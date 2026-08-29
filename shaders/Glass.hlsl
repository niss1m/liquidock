// The dock background: real liquid glass.
//
// Rather than tinting a blur, this reconstructs what you would see looking
// *through* a thin sheet of glass at whatever is behind it. A rounded-rectangle
// SDF gives the shape; its gradient gives a surface normal in a narrow band just
// inside the rim; that band bends the backdrop, splits it by wavelength, and
// catches the light.
//
// The governing idea is that glass is a *sheet*, not a dome. Everything optical
// happens in a few pixels at the edge: the middle of the panel is flat, and what
// you see through it is simply the background, blurred and lifted. Letting the
// bevel spread inward across the whole panel - and letting the sharp and blurred
// samples sit half-mixed everywhere - is what turns a pane of glass into a
// glossy balloon.
//
// Every parameter maps to a control in the preferences window, and the whole
// file hot-reloads, so tuning is edit-and-save rather than edit-and-rebuild.

#include "Common.hlsli"
#include "Sdf.hlsli"

// Matches design::kMaxLenses. Only raised icons get one, and the wave reaches
// about five icons wide, so the loop below is short in practice however many
// items the dock holds.
#define MAX_LENSES 8

cbuffer GlassConstants : register(b0)
{
    // float4s throughout: HLSL pushes a float4 to the next 16-byte boundary,
    // which silently desynchronises a mirrored C++ struct.
    float4 gViewportCenter; // xy = viewport size (px), zw = rect centre (px, window space)
    float4 gShape;          // xy = rect half size (px), z = corner radius (px), w = time (s)
    float4 gLight;          // x = angle (rad), y = intensity, z = refraction, w = depth
    float4 gMaterial;       // x = dispersion, y = frost, z = splay, w = tiled flag
    float4 gTint;           // straight alpha
    float4 gWindowOrigin;   // xy = window origin (monitor px), zw = monitor size (px)
    float4 gBackdropUv;     // xy = uv scale, zw = uv offset
    float4 gLensInfo;       // x = lens count, y = smooth-min radius (px), z = px per logical px
    // xy = centre (px), z = half width (px), w = half height (px). The corner
    // radius is min(z, w) - a lens is always as round as it can be - so it does
    // not need a slot of its own.
    float4 gLens[MAX_LENSES];
};

Texture2D gBackdrop : register(t0);
Texture2D gFrost : register(t1);
SamplerState gLinearClamp : register(s0);

#define VIEWPORT        gViewportCenter.xy
#define RECT_CENTER     gViewportCenter.zw
#define HALF_SIZE       gShape.xy
#define CORNER_RADIUS   gShape.z
#define LIGHT_ANGLE     gLight.x
#define LIGHT_INTENSITY gLight.y
#define REFRACTION      gLight.z
#define DEPTH           gLight.w
#define DISPERSION      gMaterial.x
#define SPLAY           gMaterial.z
#define TILED           gMaterial.w
#define LENS_COUNT      gLensInfo.x
#define LENS_FUSE       gLensInfo.y
#define PIXEL_SCALE     gLensInfo.z

// The edge band, in logical pixels, at depth 0 and depth 1. A sheet of glass has
// an edge you could measure with a ruler; these are that edge. The upper end is
// already thick - past it the panel stops reading as glass and starts reading as
// a bubble.
static const float kMinBandPx = 3.0;
static const float kMaxBandPx = 16.0;

// How far refraction can push a sample, as a multiple of the band width. Tied to
// the band rather than to a fixed pixel count, which is what keeps a thin edge
// looking like a thin edge: a three-pixel band displacing by fifty pixels would
// smear the whole background through a slot.
static const float kRefractionReach = 2.4;

// Wavelength spread at full dispersion, as a fraction of the refraction offset.
static const float kMaxDispersion = 0.16;

// The bright line along the rim, in logical pixels. Crisp on purpose: a wide
// soft one is a glow, and a glow is the single most Aero thing a surface can do.
static const float kRimPx = 2.0;

Varyings VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

float3 SampleBackdrop(float2 monitorPx)
{
    const float2 uv = BackdropUv(monitorPx, gWindowOrigin.zw, gBackdropUv, TILED);
    return gBackdrop.SampleLevel(gLinearClamp, uv, 0).rgb;
}

float4 PSMain(Varyings input) : SV_Target
{
    const float2 windowPx = input.position.xy;
    const float2 p = windowPx - RECT_CENTER;
    // Not named `distance`: that shadows the HLSL intrinsic.
    float dist = SdRoundedBox(p, HALF_SIZE, CORNER_RADIUS);

    // Each raised icon adds a lens, fused into the body with a smooth minimum
    // rather than a plain union. A union would leave a visible crease where the
    // two shapes cross; the smooth minimum blends them into one surface. Off by
    // default - see `icon-bulge` in the settings.
    const int lensCount = (int)LENS_COUNT;
    [loop]
    for (int i = 0; i < lensCount; ++i)
    {
        const float lens =
            SdRoundedBox(windowPx - gLens[i].xy, gLens[i].zw, min(gLens[i].z, gLens[i].w));
        dist = SmoothMin(dist, lens, LENS_FUSE);
    }

    // Derivatives must be taken before any discard. Once a lane in the quad is
    // killed its neighbours' derivatives are undefined, which shows up as a
    // torn, shimmering edge.
    const float aa = max(fwidth(dist), 1e-4);
    const float2 gradient = float2(ddx(dist), ddy(dist));

    const float coverage = 1.0 - smoothstep(-aa, aa, dist);
    if (coverage <= 0.0)
    {
        discard;
    }

    const float scale = max(PIXEL_SCALE, 0.5);

    // --- The edge band -----------------------------------------------------
    // Two parameters, two jobs. Depth is how thick the glass is: it sets the
    // base width of the band and, further down, how hard the rim bends light.
    // Splay is how far that bending fans inward from the rim, which means it
    // has to scale the band's *width* - not merely reshape the profile inside
    // it. Splay used to do only the latter, and inside a seven-pixel band an
    // exponent change is invisible: sweeping it end to end moved the render by
    // 0.6 of 765, which is to say the control did nothing.
    //
    // `edge` is 1 at the rim and falls to 0 across the band. The max() keeps the
    // pow base positive, since fxc's X3571 is fatal under warnings-as-errors.
    const float baseBand = lerp(kMinBandPx, kMaxBandPx, saturate(DEPTH)) * scale;
    const float band = baseBand * lerp(0.55, 3.2, saturate(SPLAY));
    const float inset = saturate(-dist / band);
    const float edge = pow(max(1.0 - inset, 1e-5), lerp(3.5, 1.4, saturate(SPLAY)));

    // The SDF gradient points outward, and the bevel tilts its normal the same
    // way: outward at the rim, straight at the viewer across the flat middle.
    const float2 outward = normalize(gradient + 1e-6);
    const float3 normal = normalize(float3(outward * edge, max(1.0 - edge, 0.12)));

    // --- The body ----------------------------------------------------------
    // Flat glass over a blurred background. The frost texture covers exactly
    // this window, so window space *is* its UV space. It follows only a fraction
    // of the refraction offset: at the full offset the blur smears visibly along
    // the rim, and the point of a blur is that it has no detail worth smearing.
    const float2 monitorPx = windowPx + gWindowOrigin.xy;
    // How far a sample is displaced follows the glass's thickness, not its
    // splay: a thicker edge bends light harder, a wider splay spreads the same
    // bend over more of the face. Scaling the displacement by the splayed band
    // instead would compound the two and warp the whole panel at high splay.
    const float2 offset = normal.xy * (REFRACTION * kRefractionReach * baseBand * edge);
    const float2 frostUv = (windowPx + offset * 0.3) / max(VIEWPORT, 1.0);

    float3 body = gFrost.SampleLevel(gLinearClamp, frostUv, 0).rgb;

    // Glass is not a colour filter. Pulling some saturation out and easing the
    // whole thing toward mid grey is what stops the panel simply becoming
    // whatever colour the wallpaper is, and keeps icons legible over it either
    // way. The small lift afterwards is the luminous quality real glass has: it
    // gathers light rather than merely passing it on.
    const float lum = dot(body, float3(0.2126, 0.7152, 0.0722));
    body = lerp(lum.xxx, body, 0.62);
    body = lerp(body, float3(0.5, 0.5, 0.5), 0.10);
    // Glass gathers light, and it lifts a dark background far more than a bright
    // one - which is also what keeps the panel visible as an object over black
    // and stops it blowing out over white. A flat multiply cannot do both.
    body = body + (1.0 - body) * 0.12;

    // A sheet lit from above. This gradient across the face, rather than any
    // amount of edge treatment, is what makes the surface read as a lit
    // material instead of a cut-out filled with a blur - and unlike a bevel it
    // adds no curvature, so it cannot turn the panel back into a dome.
    const float topEdge = RECT_CENTER.y - HALF_SIZE.y;
    const float vertical = saturate((windowPx.y - topEdge) / max(2.0 * HALF_SIZE.y, 1.0));
    body += (1.0 - body) * 0.09 * (1.0 - vertical);
    body *= 1.0 - 0.07 * vertical;

    // --- The lens ring -----------------------------------------------------
    // Glass disperses because its refractive index varies with wavelength, so
    // the three channels are sampled at slightly different displacements. The
    // split scales with the offset itself, which confines the fringing to the
    // rim where the bending actually happens.
    const float spread = DISPERSION * kMaxDispersion;
    const float3 refracted = float3(
        SampleBackdrop(monitorPx + offset * (1.0 + spread)).r,
        SampleBackdrop(monitorPx + offset).g,
        SampleBackdrop(monitorPx + offset * (1.0 - spread)).b);

    // The sharp, warped background shows only in the band. A ring of
    // distorted-but-legible background against a flat blurred middle is the
    // thing that actually reads as a sheet of glass; mixing the two everywhere
    // reads as fog.
    float3 colour = lerp(body, refracted, saturate(edge * 0.85));
    colour = lerp(colour, float3(1.0, 1.0, 1.0), saturate(gTint.a));

    // --- The rim -----------------------------------------------------------
    // Screen space has y pointing down, hence the sign on the second component.
    const float2 lightPlane = float2(-cos(LIGHT_ANGLE), sin(LIGHT_ANGLE));
    const float facing = dot(outward, lightPlane);

    // A crisp line just *inside* the boundary, not a broad Fresnel dome - and
    // not on the boundary itself, which is where the shape's own antialiasing
    // fades to nothing and takes the highlight with it. Sitting it a pixel and a
    // half in is the difference between a visible edge and no edge at all.
    const float rimCentre = kRimPx * 0.75 * scale;
    const float rimHalf = kRimPx * 0.7 * scale;
    const float rim = saturate(1.0 - abs(-dist - rimCentre) / max(rimHalf, 1e-4));

    const float intensity = saturate(LIGHT_INTENSITY);
    colour += rim * saturate(0.20 + 0.80 * facing) * intensity * 0.55;
    // A whisper of shadow on the side facing away. Without it the rim reads as
    // a glow around the whole shape rather than as light landing on an edge.
    colour *= 1.0 - rim * saturate(-facing) * 0.30 * intensity;

    // A second, much softer treatment inside the rim: a sheen where the surface
    // turns upward and a shadow where it turns down, both falling off over a
    // dozen pixels. The crisp rim alone gives an outlined shape; this is what
    // gives it thickness. It follows the distance field, so it wraps the corners
    // and any bulge instead of being a band across the top.
    const float inner = pow(saturate(1.0 + dist / (16.0 * scale)), 2.0);
    colour += saturate(-outward.y) * inner * 0.075 * intensity;
    colour *= 1.0 - saturate(outward.y) * inner * 0.10 * intensity;

    // The backdrop has already been sampled and composited into `colour`, so
    // the dock is opaque wherever it covers - it is showing its own
    // reconstruction of what is behind it, not blending with it.
    const float alpha = coverage;

    // The composition swap chain is premultiplied; straight alpha is not a
    // legal alpha mode for it.
    return float4(saturate(colour) * alpha, alpha);
}
