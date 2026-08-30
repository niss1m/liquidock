// The icon row, and the hairline between the two groups.
//
// One instanced draw for the whole row. Six vertices per instance generated
// from SV_VertexID, everything else read out of a constant-buffer array indexed
// by SV_InstanceID - so there is no vertex buffer, no index buffer and no input
// layout, and adding an item costs one more instance rather than one more draw.
//
// Instances flagged solid are filled with white rather than sampled from the
// atlas. That is how the hairline rides along in the same draw: it is a rect
// with an opacity, and giving it a pass of its own would double the state
// changes for a one-pixel line.

#include "Common.hlsli"

// design::kMaxItems icons, the same again for running indicators, plus the
// hairline. Matches kMaxIconInstances in DockWindow.h.
#define MAX_INSTANCES 129

cbuffer IconConstants : register(b0)
{
    float4 gViewport; // xy = viewport size (px), zw = 1 / viewport size
    float4 gCell;     // xy = one atlas cell in uv, zw unused
    // What a solid instance is filled with. White on a dark theme, near-black
    // on a light one - the hairline between runs and the dot under a running
    // app both have to be visible against whatever the glass is over, and on a
    // light desktop a white rule is not.
    float4 gInk;
    // xy = centre (px), zw = half size (px)
    float4 gRect[MAX_INSTANCES];
    // xy = atlas cell origin in uv, z = opacity,
    // w = 0 atlas sample, 1 solid rect, 2 solid circle
    float4 gSource[MAX_INSTANCES];
};

Texture2D gIcons : register(t0);
SamplerState gLinearClamp : register(s0);

struct IconVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    // HLSL has no `flat` qualifier, so the instance's own scalars ride along
    // per vertex. They are constant across the triangle, so interpolating them
    // is exact rather than merely close.
    float2 fill : TEXCOORD1;  // x = opacity, y = mode
    float2 local : TEXCOORD2; // 0..1 across the quad, for the circle mode
};

IconVaryings VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    // Two triangles from six vertex ids, as a unit quad in 0..1. Spelled out
    // rather than derived: the winding has to come out clockwise in clip space
    // or the default rasteriser state culls every icon, and an arithmetic trick
    // that gets it backwards fails silently as an empty dock.
    static const uint kCorners[6] = {0, 1, 2, 0, 2, 3};
    const uint corner = kCorners[vertexId];
    const float2 unit = float2((corner == 1 || corner == 2) ? 1.0 : 0.0,
                               (corner >= 2) ? 1.0 : 0.0);

    const float4 rect = gRect[instanceId];
    const float4 source = gSource[instanceId];

    const float2 pixel = rect.xy + (unit * 2.0 - 1.0) * rect.zw;

    IconVaryings output;
    // Pixel space to clip space. Y flips because screen space grows downward.
    output.position =
        float4(pixel * gViewport.zw * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = source.xy + unit * gCell.xy;
    output.fill = float2(source.z, source.w);
    output.local = unit;
    return output;
}

float4 PSMain(IconVaryings input) : SV_Target
{
    // The atlas holds premultiplied BGRA and the swap chain is premultiplied,
    // so a sample needs nothing done to it but a fade.
    float4 colour = gIcons.Sample(gLinearClamp, input.uv);

    // Solid instances ignore the atlas entirely: the ink colour, with the
    // opacity carrying the tint's alpha. The hairline and the running
    // indicators.
    colour = lerp(colour, float4(gInk.rgb, 1.0), saturate(input.fill.y));

    // Mode 2 clips that solid fill to a disc. A four-pixel square would read as
    // a speck of dirt rather than as a light under the icon, and a dot is not
    // worth its own shader.
    if (input.fill.y > 1.5)
    {
        const float2 offset = input.local * 2.0 - 1.0;
        const float radius = length(offset);
        const float aa = max(fwidth(radius), 1e-4);
        colour *= 1.0 - smoothstep(1.0 - aa, 1.0, radius);
    }

    return colour * input.fill.x;
}
