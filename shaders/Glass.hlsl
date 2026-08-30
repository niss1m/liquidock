// The dock background: real liquid glass.
//
// Rather than tinting a blur, this reconstructs what you would see looking
// *through* a thin sheet of glass at whatever is behind it. A rounded-rectangle
// SDF gives the shape; its gradient gives a surface normal in a band inside the
// rim; that band bends what is behind the pane, splits it by wavelength, and
// catches the light.
//
// Everything the pane looks through comes from one texture: the frost chain's
// output, which is the backdrop cropped to this window and blurred by the frost
// setting. That is not a shortcut, it is the physics - frosting is a property
// of the material, so light bent at the rim has passed through the same glass
// as light crossing the middle. The earlier version sampled a *sharp* backdrop
// for the rim and a blurred one for the middle and crossfaded between them,
// which meant turning refraction up quietly cancelled frost: at the settings
// this dock shipped with, a frost of 1.00 was re-sharpened back to nothing
// across the whole panel and the two controls fought each other.
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
    float4 gMaterial;       // x = dispersion, y = frost, z = splay, w = unused
    float4 gTint;           // straight alpha
    float4 gLensInfo;       // x = lens count, y = smooth-min radius (px), z = px per logical px
    // xy = centre (px), z = half width (px), w = half height (px). The corner
    // radius is min(z, w) - a lens is always as round as it can be - so it does
    // not need a slot of its own.
    float4 gLens[MAX_LENSES];
};

// The backdrop, cropped to this window and frosted. Window space is its UV
// space, which is why nothing in here needs to know about monitors, wallpaper
// fit modes or tiling: the frost chain resolved all of that already.
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
#define LENS_COUNT      gLensInfo.x
#define LENS_FUSE       gLensInfo.y
#define PIXEL_SCALE     gLensInfo.z

// How far the bending fans inward, as a fraction of the panel's own half
// height, at splay 0 and splay 1. A fraction rather than a pixel count is the
// whole point: the previous version measured the band in logical pixels, so on
// this dock's 45 px bar it was already past the middle at half splay and the
// control had nothing left to do. Sweeping it end to end changed the render by
// less than a part in a thousand, which is why it read as having no effect.
static const float kMinBandFraction = 0.08;
static const float kMaxBandFraction = 1.00;

// How far the rim can displace what it is looking at, in logical pixels, at
// full depth. Depth is the thickness of the sheet, and a thicker sheet bends
// harder; the design's depth of 0.20 lands at about four pixels of squeeze at
// the rim, which is what its own render measures.
static const float kMaxBendPx = 20.0;

// Wavelength spread at full dispersion, as a fraction of the bend.
static const float kMaxDispersion = 0.16;

// The rim, in logical pixels. The design's render puts it at *one* pixel of a
// 2.25x export - 0.44 logical, genuinely sub-pixel - with its neighbours only
// partly lit. A wide soft one is a glow, and a glow is the single most Aero
// thing a surface can do.
static const float kRimPx = 0.8;

// How far the rim lifts what is behind it toward white, at the light intensity
// the design states (0.80), and how much of that leans with the light.
//
// Sampling the design's rim gives RGB 153/173/130 over a backdrop of 48/70/24.
// Solve that per channel and it is a lerp toward white of 0.51, 0.56, 0.46 -
// one number, near enough, and emphatically not an addition. Around the
// perimeter the rim lands at 152 at the top, 144 and 124 at the sides and 119
// at the bottom, so it is a bright edge the whole way round that leans toward
// the light rather than a highlight on one side.
static const float kRimLift = 0.56;
static const float kRimLean = 0.09;

Varyings VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

float3 LookThrough(float2 windowPx)
{
    return gFrost.SampleLevel(gLinearClamp, windowPx / max(VIEWPORT, 1.0), 0).rgb;
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

    // --- The bevel ---------------------------------------------------------
    // Two parameters with two jobs, and they are now genuinely different ones.
    // Splay is *how far in* the bending reaches; depth is *how hard* it bends.
    // Previously both widened the band, so at any high setting one of them had
    // nothing left to move.
    //
    // `edge` is 1 at the rim and falls to 0 across the band. The max() keeps the
    // pow base positive, since fxc's X3571 is fatal under warnings-as-errors.
    const float halfMin = max(min(HALF_SIZE.x, HALF_SIZE.y), 1.0);
    const float band = halfMin * lerp(kMinBandFraction, kMaxBandFraction, saturate(SPLAY));
    const float inset = saturate(-dist / band);
    const float edge = pow(max(1.0 - inset, 1e-5), lerp(3.0, 1.15, saturate(SPLAY)));

    // The SDF gradient points outward, and the bevel tilts its normal the same
    // way: outward at the rim, straight at the viewer across the flat middle.
    const float2 outward = normalize(gradient + 1e-6);
    const float3 normal = normalize(float3(outward * edge, max(1.0 - edge, 0.12)));

    // --- Looking through ---------------------------------------------------
    // The sample walks *outward* as it approaches the rim, so the rim shows a
    // squeezed band of what lies just outside the panel. That compression is
    // what a real edge does and it is the single strongest cue that the shape
    // is a solid piece of glass rather than a translucent rectangle.
    const float bend = REFRACTION * lerp(1.0, kMaxBendPx, saturate(DEPTH)) * scale;
    const float2 offset = outward * (bend * edge * edge);

    // Glass disperses because its refractive index varies with wavelength, so
    // the three channels are sampled at slightly different displacements. The
    // split scales with the offset itself, which confines the fringing to the
    // rim where the bending actually happens.
    const float spread = DISPERSION * kMaxDispersion;
    float3 colour = float3(
        LookThrough(windowPx + offset * (1.0 + spread)).r,
        LookThrough(windowPx + offset).g,
        LookThrough(windowPx + offset * (1.0 - spread)).b);

    colour = lerp(colour, float3(1.0, 1.0, 1.0), saturate(gTint.a));

    // A sheet lit from above: a whisper brighter at the top than the bottom.
    // Measured off the design's own render this is small - its interior is the
    // backdrop, blurred, and very little else - so what used to be a tenth of a
    // stop of lift and a desaturation is now barely a fortieth. That lift is
    // what a previous pass turned up chasing "glassiness" and what made the
    // dock read as a smudge.
    const float topEdge = RECT_CENTER.y - HALF_SIZE.y;
    const float vertical = saturate((windowPx.y - topEdge) / max(2.0 * HALF_SIZE.y, 1.0));
    colour += (1.0 - colour) * 0.025 * (1.0 - vertical);
    colour *= 1.0 - 0.02 * vertical;

    // --- The rim -----------------------------------------------------------
    // Screen space has y pointing down, hence the sign on the second component.
    const float2 lightPlane = float2(-cos(LIGHT_ANGLE), sin(LIGHT_ANGLE));
    const float facing = dot(outward, lightPlane);

    // A crisp line just *inside* the boundary, not a broad Fresnel dome - and
    // not on the boundary itself, where the shape's own antialiasing fades to
    // nothing and takes the highlight with it.
    //
    // The weights are measured, not guessed. Sampling the design's render
    // across its rim gives +100/255 at the top edge, +83 at both sides and +59
    // at the bottom, at its stated light intensity of 0.80 - a bright line the
    // whole way round that only leans towards the light rather than a highlight
    // that appears on one side and a shadow on the other. An earlier version
    // had it at 0.20 on the unlit side, which is a quarter of what the design
    // shows, and is why the bottom of the bar had no edge to it at all.
    const float rimCentre = kRimPx * 0.55 * scale;
    const float rimHalf = kRimPx * 0.75 * scale;
    const float rim = saturate(1.0 - abs(-dist - rimCentre) / max(rimHalf, 1e-4));

    // A *reflection*, not a stroke. Adding a constant is what makes an edge
    // read as painted on: over a dark backdrop it is a grey line, over a bright
    // one it clips to flat white, and either way its brightness is decided by
    // what is underneath rather than by what it is reflecting. Lifting toward
    // white lands the rim at the same value whatever is behind it - which is
    // what a specular highlight does, and the whole reason glass looks like
    // glass rather than like a rectangle with a border.
    const float intensity = saturate(LIGHT_INTENSITY);
    const float lift = saturate(intensity * (kRimLift + kRimLean * facing));
    colour = lerp(colour, float3(1.0, 1.0, 1.0), rim * lift);

    // A far softer sheen inside the rim where the surface turns towards the
    // light, falling off over a dozen pixels. It follows the distance field, so
    // it wraps the corners and any bulge instead of being a band across the top.
    // Also a lift rather than an addition, for the same reason.
    const float inner = pow(saturate(1.0 + dist / (16.0 * scale)), 2.0);
    colour = lerp(colour, float3(1.0, 1.0, 1.0),
                  saturate(-outward.y) * inner * 0.05 * intensity);

    // The backdrop has already been sampled and composited into `colour`, so
    // the dock is opaque wherever it covers - it is showing its own
    // reconstruction of what is behind it, not blending with it.
    const float alpha = coverage;

    // The composition swap chain is premultiplied; straight alpha is not a
    // legal alpha mode for it.
    return float4(saturate(colour) * alpha, alpha);
}
