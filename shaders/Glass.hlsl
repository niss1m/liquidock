// The dock background.
//
// M0 scope: prove the transparency path end to end - a rounded rect with a
// bevel and a light-driven rim, composited per-pixel against the desktop. The
// constant buffer already carries the full parameter set from the Figma glass
// panel (light angle and intensity, refraction, depth, dispersion, frost,
// splay) so M1 is a change to this file alone: bind the backdrop and the
// blurred backdrop, then bend the sample through the normal computed below.

#include "Sdf.hlsli"

cbuffer GlassConstants : register(b0)
{
    // Packed as float4s deliberately. HLSL's scalar packing rules push a float4
    // to the next 16-byte boundary, which silently desynchronises a mirrored
    // C++ struct; float4s throughout make the layout unambiguous.
    float4 gViewportCenter;  // xy = viewport size (px), zw = rect centre (px)
    float4 gShape;           // xy = rect half size (px), z = corner radius (px), w = time (s)
    float4 gLight;           // x = angle (rad), y = intensity, z = refraction, w = depth
    float4 gMaterial;        // x = dispersion, y = frost, z = splay, w = reserved
    float4 gTint;            // straight alpha
};

#define RECT_CENTER     gViewportCenter.zw
#define HALF_SIZE       gShape.xy
#define CORNER_RADIUS   gShape.z
#define LIGHT_ANGLE     gLight.x
#define LIGHT_INTENSITY gLight.y
#define DEPTH           gLight.w
#define SPLAY           gMaterial.z

struct Varyings
{
    float4 position : SV_Position;
};

// Fullscreen triangle from the vertex id alone - no vertex or index buffer, so
// there is no input layout to bind and nothing to keep resident.
Varyings VSMain(uint id : SV_VertexID)
{
    Varyings output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain(Varyings input) : SV_Target
{
    const float2 p = input.position.xy - RECT_CENTER;
    // Not named `distance`: that shadows the HLSL intrinsic of the same name.
    const float dist = SdRoundedBox(p, HALF_SIZE, CORNER_RADIUS);

    // Every derivative has to be taken before the discard below. Once a lane in
    // the quad is killed its neighbours' derivatives are undefined, and the
    // result is a torn, shimmering edge.
    const float aa = max(fwidth(dist), 1e-4);
    const float2 gradient = float2(ddx(dist), ddy(dist));

    const float coverage = 1.0 - smoothstep(-aa, aa, dist);
    if (coverage <= 0.0)
    {
        discard;
    }

    // Bevel profile. Depth sets how thick the glass edge reads; splay sets how
    // far that thickness bleeds inward before the surface goes flat.
    const float bevelWidth = lerp(2.0, 28.0, saturate(DEPTH));
    const float inset = saturate(-dist / bevelWidth);
    // The max() guards keep the base of every pow() strictly positive. Without
    // them fxc emits X3571, which warnings-as-errors turns into a build break.
    const float height = 1.0 - pow(max(1.0 - inset, 1e-5), lerp(1.0, 4.0, saturate(SPLAY)));

    // The SDF gradient points outward, away from the shape. A convex bevel
    // tilts its normal the same way - outward and toward the viewer - so the
    // slope is added, not negated. Negating it puts the highlight on the rim
    // opposite the light, and would send M1's refraction the wrong way too.
    const float2 slope = normalize(gradient + 1e-6) * (1.0 - height);
    const float3 normal = normalize(float3(slope, 0.35));

    // Negating cosine puts the highlight on the top-left rim at -45 degrees,
    // matching the light puck in the Figma glass panel.
    const float2 lightDir = float2(-cos(LIGHT_ANGLE), sin(LIGHT_ANGLE));
    const float rim = pow(max(dot(normal.xy, lightDir), 1e-5), 6.0);
    const float specular = rim * (1.0 - height) * saturate(LIGHT_INTENSITY);

    // The rim highlight is emissive, so it has to be added *after*
    // premultiplication rather than folded into the tint colour. The design
    // tint is pure white, so `saturate(gTint.rgb + specular)` would clamp to 1
    // no matter what the lighting did - every term above would be dead code.
    //
    // Adding light raises alpha along with colour, which is what keeps the
    // premultiplied invariant rgb <= a intact.
    const float baseAlpha = gTint.a * coverage;
    const float glow = saturate(specular) * coverage;
    const float alpha = saturate(baseAlpha + glow);
    const float3 premultiplied = gTint.rgb * baseAlpha + glow;

    // The composition swap chain is premultiplied; straight alpha is not a
    // legal alpha mode for it.
    return float4(premultiplied, alpha);
}
