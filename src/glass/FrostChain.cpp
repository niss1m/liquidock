#include "glass/FrostChain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {
namespace {

// Reducing by N and sampling back up is itself a blur of roughly N/2.6 pixels -
// the cost of the bilinear filter on the way down and again on the way up. So
// a divisor is only free once the blur it is helping with is comfortably wider
// than that, and running at a quarter unconditionally put a floor of about a
// pixel and a half under a setting whose whole design range starts below two.
//
// This keeps the resampling error under about a fifth of the radius being
// asked for, which is imperceptible, and still drops to a quarter for the
// heavy frosts where the bandwidth actually matters.
constexpr float kSigmaPerDivisor = 2.6f;
constexpr UINT kMaxDownscale = 4;

} // namespace

UINT FrostChain::DownscaleFor(float sigmaPx) {
    const float allowed = sigmaPx / kSigmaPerDivisor;
    UINT divisor = 1;
    while (divisor < kMaxDownscale && static_cast<float>(divisor + 1) <= allowed) {
        ++divisor;
    }
    return divisor;
}

bool FrostChain::Initialize(GraphicsDevice& device, ShaderCache& shaders) {
    device_ = &device;
    shaders_ = &shaders;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(BlurConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    LD_CHECK(device_->d3d()->CreateBuffer(&desc, nullptr, &constantBuffer_));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    LD_CHECK(device_->d3d()->CreateSamplerState(&sampler, &sampler_));
    return true;
}

void FrostChain::Reset() {
    cropped_.Reset();
    temp_.Reset();
    frost_.Reset();
    sampler_.Reset();
    constantBuffer_.Reset();
    fullWidth_ = 0;
    fullHeight_ = 0;
    width_ = 0;
    height_ = 0;
    downscale_ = 0;
}

bool FrostChain::CreateTarget(Target& target, UINT width, UINT height) {
    target.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    LD_CHECK(device_->d3d()->CreateTexture2D(&desc, nullptr, &target.texture));
    LD_CHECK(device_->d3d()->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv));
    LD_CHECK(device_->d3d()->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv));
    return true;
}

bool FrostChain::Resize(UINT windowWidth, UINT windowHeight) {
    if (windowWidth == fullWidth_ && windowHeight == fullHeight_) {
        return true;
    }
    fullWidth_ = windowWidth;
    fullHeight_ = windowHeight;
    // Force the next Build to reallocate: the divisor it wants may not have
    // changed, but the size underneath it has.
    downscale_ = 0;
    return true;
}

bool FrostChain::EnsureTargets(UINT downscale) {
    if (downscale == downscale_ && frost_.srv) {
        return true;
    }

    const UINT width = std::max<UINT>(1, fullWidth_ / downscale);
    const UINT height = std::max<UINT>(1, fullHeight_ / downscale);
    if (!CreateTarget(cropped_, width, height) || !CreateTarget(temp_, width, height) ||
        !CreateTarget(frost_, width, height)) {
        return false;
    }

    width_ = width;
    height_ = height;
    downscale_ = downscale;
    LogDebug("Frost chain {}x{} at 1/{}", width, height, downscale);
    return true;
}

void FrostChain::RunPass(Target& destination, ID3D11ShaderResourceView* source,
                         const char* entryPoint, const BlurConstants& constants) {
    ComPtr<ID3D11VertexShader> vs = shaders_->VertexShader("Blur", "VSMain");
    ComPtr<ID3D11PixelShader> ps = shaders_->PixelShader("Blur", entryPoint);
    if (!vs || !ps) {
        return;
    }

    ID3D11DeviceContext1* ctx = device_->context();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ctx->Unmap(constantBuffer_.Get(), 0);

    // Unbind the source from the pixel stage before binding it as an output, or
    // the debug layer complains about the resource being bound for read and
    // write simultaneously and silently drops one of them.
    ID3D11ShaderResourceView* nullSrv[1] = {nullptr};
    ctx->PSSetShaderResources(0, 1, nullSrv);

    ID3D11RenderTargetView* rtv = destination.rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, constants.target[0], constants.target[1], 0.0f, 1.0f};
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);

    ID3D11Buffer* cb = constantBuffer_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &source);
    // Bound explicitly rather than left to the default sampler: the default
    // wraps, so the blur's outermost taps would pull in the opposite edge of the
    // backdrop instead of clamping to the nearest one.
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv[1] = {nullptr};
    ctx->OMSetRenderTargets(1, nullRtv, nullptr);
    ctx->PSSetShaderResources(0, 1, nullSrv);
}

bool FrostChain::Build(const Backdrop& backdrop, POINT windowOrigin, SIZE windowSize,
                       float sigmaPx) {
    if (!backdrop.srv() || fullWidth_ == 0) {
        return false;
    }
    if (!EnsureTargets(DownscaleFor(sigmaPx))) {
        return false;
    }

    const RECT monitor = backdrop.monitor_rect();
    float uvScale[2]{};
    float uvOffset[2]{};
    backdrop.uv_scale(uvScale);
    backdrop.uv_offset(uvOffset);

    BlurConstants constants{};
    constants.target[0] = static_cast<float>(width_);
    constants.target[1] = static_cast<float>(height_);
    constants.target[2] = 1.0f / static_cast<float>(width_);
    constants.target[3] = 1.0f / static_cast<float>(height_);
    constants.window[0] = static_cast<float>(windowOrigin.x);
    constants.window[1] = static_cast<float>(windowOrigin.y);
    constants.window[2] = static_cast<float>(windowSize.cx);
    constants.window[3] = static_cast<float>(windowSize.cy);
    constants.monitor[0] = static_cast<float>(monitor.right - monitor.left);
    constants.monitor[1] = static_cast<float>(monitor.bottom - monitor.top);
    // Sigma is quoted in full-resolution pixels but applied to a reduced
    // target, so it has to come down by the same factor. The resampling either
    // side of the blur contributes its own width, and subtracting it in
    // quadrature - blurs compose as the root of the sum of squares, not the sum -
    // is what keeps the result the radius that was asked for rather than that
    // radius plus whatever the divisor cost.
    const float divisor = static_cast<float>(downscale_);
    const float resampling = (divisor > 1.0f) ? (divisor / kSigmaPerDivisor) : 0.0f;
    const float corrected =
        std::sqrt(std::max(sigmaPx * sigmaPx - resampling * resampling, 0.0f));
    constants.monitor[2] = corrected / divisor;
    constants.monitor[3] = backdrop.tiled() ? 1.0f : 0.0f;
    constants.backdropUv[0] = uvScale[0];
    constants.backdropUv[1] = uvScale[1];
    constants.backdropUv[2] = uvOffset[0];
    constants.backdropUv[3] = uvOffset[1];

    // Pass 1: crop the backdrop to the window footprint.
    RunPass(cropped_, backdrop.srv(), "PSDownsample", constants);

    // Pass 2: horizontal.
    constants.direction[0] = constants.target[2];
    constants.direction[1] = 0.0f;
    RunPass(temp_, cropped_.srv.Get(), "PSBlur", constants);

    // Pass 3: vertical.
    constants.direction[0] = 0.0f;
    constants.direction[1] = constants.target[3];
    RunPass(frost_, temp_.srv.Get(), "PSBlur", constants);

    return true;
}

} // namespace liquidock
