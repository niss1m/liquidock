#ifndef LIQUIDOCK_COMMON_HLSLI
#define LIQUIDOCK_COMMON_HLSLI

struct Varyings
{
    float4 position : SV_Position;
};

// Fullscreen triangle from the vertex id alone - no vertex or index buffer, so
// there is no input layout to bind and nothing to keep resident. Winding is
// clockwise in screen space, which is front-facing under D3D's default
// rasteriser state.
Varyings FullscreenTriangle(uint id)
{
    Varyings output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// Maps a point in monitor-relative pixels to a UV in the wallpaper.
//
// The wallpaper is uploaded at its native size and the desktop's fit mode
// (fill, fit, stretch, centre, tile) is reduced on the CPU to this scale and
// offset, so every mode costs the same here and only tiling needs a branch.
float2 BackdropUv(float2 monitorPx, float2 monitorSize, float4 uvTransform, float tiled)
{
    const float2 t = monitorPx / max(monitorSize, 1.0);
    const float2 uv = t * uvTransform.xy + uvTransform.zw;
    return (tiled > 0.5) ? frac(uv) : uv;
}

#endif // LIQUIDOCK_COMMON_HLSLI
