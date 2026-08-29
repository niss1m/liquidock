// Builds the frosted copy of the backdrop that sits behind the dock.
//
// Three passes: crop the window's region out of the wallpaper at quarter
// resolution, then a separable Gaussian across and down. The result is a
// texture roughly 190x40, which is why this is affordable at all.
//
// Crucially this is *cached*. It re-runs only when the wallpaper changes, the
// dock moves or resizes, or the frost amount is edited - not per frame. On a
// static desktop it runs zero times per second, which is what lets the dock
// idle at no cost while still refracting a blurred backdrop.

#include "Common.hlsli"

cbuffer BlurConstants : register(b0)
{
    float4 gTarget;     // xy = target size (px), zw = 1 / target size
    float4 gWindow;     // xy = window origin (monitor px), zw = window size (px)
    float4 gMonitor;    // xy = monitor size (px), z = sigma (texels), w = tiled flag
    float4 gBackdropUv; // xy = uv scale, zw = uv offset
    float4 gDirection;  // xy = per-tap uv step for this pass
};

Texture2D gSource : register(t0);
SamplerState gLinearClamp : register(s0);

Varyings VSMain(uint id : SV_VertexID)
{
    return FullscreenTriangle(id);
}

// Pass 1: crop the wallpaper down to just the window's footprint.
float4 PSDownsample(Varyings input) : SV_Target
{
    const float2 t = input.position.xy * gTarget.zw;
    const float2 monitorPx = gWindow.xy + t * gWindow.zw;
    const float2 uv = BackdropUv(monitorPx, gMonitor.xy, gBackdropUv, gMonitor.w);
    return float4(gSource.SampleLevel(gLinearClamp, uv, 0).rgb, 1.0);
}

// Passes 2 and 3: one axis of a separable Gaussian.
float4 PSBlur(Varyings input) : SV_Target
{
    const float2 uv = input.position.xy * gTarget.zw;
    const float sigma = gMonitor.z;

    // Below about a quarter of a texel the kernel collapses to a single tap and
    // the taps either side contribute nothing measurable, so skip the work.
    if (sigma < 0.25)
    {
        return float4(gSource.SampleLevel(gLinearClamp, uv, 0).rgb, 1.0);
    }

    const float denom = 2.0 * sigma * sigma;
    float3 accumulated = 0.0;
    float weightSum = 0.0;

    [unroll]
    for (int i = -6; i <= 6; ++i)
    {
        const float weight = exp(-(i * i) / denom);
        accumulated += gSource.SampleLevel(gLinearClamp, uv + i * gDirection.xy, 0).rgb * weight;
        weightSum += weight;
    }

    return float4(accumulated / max(weightSum, 1e-5), 1.0);
}
