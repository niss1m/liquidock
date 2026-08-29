// The dock background: real liquid glass.
//
// Rather than tinting a blur, this reconstructs what you would see looking
// *through* a slab of glass at the desktop behind it. A rounded-rectangle SDF
// gives a bevel profile; the gradient of that profile gives a surface normal;
// the normal bends the backdrop sample (refraction), splits it by wavelength at
// the rim (dispersion), and drives both the specular highlight and the Fresnel
// edge. Frost mixes in a pre-blurred copy of the same backdrop.
//
// Every parameter maps to a control in the Figma glass panel, and the whole
// file hot-reloads, so tuning is edit-and-save rather than edit-and-rebuild.

#include "Common.hlsli"
#include "Sdf.hlsli"

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
#define FROST           gMaterial.y
#define SPLAY           gMaterial.z
#define TILED           gMaterial.w

// How far, in pixels, refraction can displace a sample at full strength. Tuned
// against the Figma render: much beyond this and the rim smears rather than
// bending, which reads as a fisheye lens instead of glass.
static const float kMaxRefractionPx = 46.0;

// Wavelength spread at full dispersion, as a fraction of the refraction offset.
static const float kMaxDispersion = 0.16;

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
    const float dist = SdRoundedBox(p, HALF_SIZE, CORNER_RADIUS);

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

    // --- Bevel -------------------------------------------------------------
    // Depth sets how thick the glass edge reads; splay sets how far that
    // thickness bleeds inward before the surface flattens off.
    const float bevelWidth = lerp(4.0, 40.0, saturate(DEPTH));
    const float inset = saturate(-dist / bevelWidth);
    // `edge` is 1 at the rim and falls to 0 across the bevel. Splay controls
    // how far the bending fans inward, so high splay means a *slower* falloff:
    // the exponent goes down as splay goes up, not up. The max() keeps the pow
    // base positive, since fxc's X3571 is fatal under warnings-as-errors.
    const float edge = pow(max(1.0 - inset, 1e-5), lerp(4.0, 1.0, saturate(SPLAY)));

    // The SDF gradient points outward, and a convex bevel tilts its normal the
    // same way: outward at the rim, straight at the viewer in the middle.
    const float2 outward = normalize(gradient + 1e-6);
    const float3 normal = normalize(float3(outward * edge, max(1.0 - edge, 0.15)));

    // --- Refraction and dispersion -----------------------------------------
    const float2 monitorPx = windowPx + gWindowOrigin.xy;
    const float2 offset = normal.xy * (REFRACTION * kMaxRefractionPx * edge);

    // Glass disperses because the refractive index varies with wavelength, so
    // the three channels are sampled at slightly different displacements. The
    // split scales with the offset itself, which confines the fringing to the
    // rim where the bending actually happens.
    const float spread = DISPERSION * kMaxDispersion;
    const float3 refracted = float3(
        SampleBackdrop(monitorPx + offset * (1.0 + spread)).r,
        SampleBackdrop(monitorPx + offset).g,
        SampleBackdrop(monitorPx + offset * (1.0 - spread)).b);

    // --- Frost -------------------------------------------------------------
    // The frost texture covers exactly this window, so window space *is* its
    // UV space. It follows the same refraction offset as the sharp sample, or
    // the two would visibly slide apart at the rim.
    const float2 frostUv = (windowPx + offset) / max(VIEWPORT, 1.0);
    const float3 frosted = gFrost.SampleLevel(gLinearClamp, frostUv, 0).rgb;
    // Thicker glass scatters more, so frost leans on the bevel.
    const float frostMix = saturate(FROST * (0.65 + 0.85 * edge));

    float3 colour = lerp(refracted, frosted, frostMix);
    colour = lerp(colour, gTint.rgb, saturate(gTint.a));

    // --- Light -------------------------------------------------------------
    // Negating cosine puts the highlight on the top-left rim at -45 degrees,
    // matching the light puck in the Figma glass panel. Screen space has y
    // pointing down, hence the sign on the second component.
    const float2 lightPlane = float2(-cos(LIGHT_ANGLE), sin(LIGHT_ANGLE));
    const float3 lightDir = normalize(float3(lightPlane * 0.82, 0.57));
    const float3 viewDir = float3(0.0, 0.0, 1.0);
    const float3 halfway = normalize(lightDir + viewDir);

    const float specular = pow(max(dot(normal, halfway), 1e-5), 64.0) * saturate(LIGHT_INTENSITY);

    // Fresnel: a surface turning away from the viewer reflects more. This is
    // what draws the thin bright line around the whole rim, independent of
    // where the light is.
    const float fresnel = pow(max(1.0 - normal.z, 1e-5), 3.0) * 0.35 * saturate(LIGHT_INTENSITY);

    colour += specular + fresnel;

    // The backdrop has already been sampled and composited into `colour`, so
    // the dock is opaque wherever it covers - it is showing its own
    // reconstruction of what is behind it, not blending with it.
    const float alpha = coverage;

    // The composition swap chain is premultiplied; straight alpha is not a
    // legal alpha mode for it.
    return float4(saturate(colour) * alpha, alpha);
}
