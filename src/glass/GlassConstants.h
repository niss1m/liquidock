#pragma once

#include "core/DesignTokens.h"

namespace liquidock {

// Mirrors GlassConstants in shaders/Glass.hlsl. Both sides are float4-packed so
// the layouts cannot drift apart silently.
struct GlassConstants {
    float viewportCenter[4]; // xy = viewport size (px), zw = rect centre (px)
    float shape[4];          // xy = rect half size (px), z = corner radius (px), w = time (s)
    float light[4];          // x = angle (rad), y = intensity, z = refraction, w = depth
    float material[4];       // x = dispersion, y = frost, z = splay, w = tiled flag
    float tint[4];           // straight alpha
    float windowOrigin[4];   // xy = window origin (monitor px), zw = monitor size (px)
    float backdropUv[4];     // xy = uv scale, zw = uv offset
    float lensInfo[4];       // x = lens count, y = smooth-min radius (px)
    float lens[design::kMaxLenses][4]; // xy = centre (px), zw = half size (px)
};
static_assert(sizeof(GlassConstants) == 256, "GlassConstants must match the HLSL cbuffer");
static_assert(sizeof(GlassConstants) % 16 == 0, "Constant buffers must be 16-byte aligned");

} // namespace liquidock
