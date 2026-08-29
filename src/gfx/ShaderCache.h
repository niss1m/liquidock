#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <unordered_map>

namespace liquidock {

using Microsoft::WRL::ComPtr;

// Compiles HLSL at runtime and caches the result.
//
// Runtime compilation rather than offline fxc is a deliberate trade: it costs a
// few milliseconds at startup and buys hot reload, which is what makes tuning
// the glass tractable. Save a .hlsl and the dock re-renders with it. Release
// builds read the source embedded at build time; debug builds prefer the file
// on disk so the watcher has something to watch.
class ShaderCache {
public:
    explicit ShaderCache(ID3D11Device1* device) : device_(device) {}

    ComPtr<ID3D11VertexShader> VertexShader(std::string_view name, const char* entryPoint);
    ComPtr<ID3D11PixelShader> PixelShader(std::string_view name, const char* entryPoint);

    // Returns true when a shader changed on disk since the previous call,
    // having already dropped every cached shader. The caller re-fetches what it
    // needs and redraws. Always false in release builds.
    bool PollForChanges();

    // Resolves a shader file by name, extension included. Public because the
    // include handler needs it too.
    static std::string LoadSource(std::string_view filename);

private:
    ComPtr<ID3DBlob> Compile(std::string_view name, const char* entryPoint, const char* profile);

    ID3D11Device1* device_;
    std::unordered_map<std::string, ComPtr<ID3D11VertexShader>> vertexShaders_;
    std::unordered_map<std::string, ComPtr<ID3D11PixelShader>> pixelShaders_;
    unsigned long long sourceStamp_ = 0;
};

} // namespace liquidock
