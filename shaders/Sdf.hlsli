#ifndef LIQUIDOCK_SDF_HLSLI
#define LIQUIDOCK_SDF_HLSLI

// Signed distance to an axis-aligned rounded box centred on the origin.
// Negative inside, positive outside, and metric everywhere - which is what lets
// the glass pass derive a bevel height and a surface normal from one evaluation
// instead of rasterising geometry.
float SdRoundedBox(float2 p, float2 halfSize, float radius)
{
    float2 q = abs(p) - halfSize + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

// Polynomial smooth minimum. M2 fuses the dock body with a lens per magnified
// icon through this, so the glass bulges around a raised icon rather than
// intersecting it with a visible seam.
float SmoothMin(float a, float b, float k)
{
    float h = saturate(0.5 + 0.5 * (b - a) / max(k, 1e-5));
    return lerp(b, a, h) - k * h * (1.0 - h);
}

#endif // LIQUIDOCK_SDF_HLSLI
