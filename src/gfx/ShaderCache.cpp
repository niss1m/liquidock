#include "gfx/ShaderCache.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "EmbeddedShaders.h"
#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {
namespace {

// Resolves #include directives against the same sources the compiler is fed,
// so a shared Sdf.hlsli works identically whether it came from disk or from the
// embedded table.
class IncludeHandler final : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, LPCVOID* data,
                           UINT* bytes) override {
        auto source = std::make_unique<std::string>(ShaderCache::LoadSource(fileName));
        if (source->empty()) {
            LogError("Shader include not found: {}", fileName);
            return E_FAIL;
        }
        *data = source->data();
        *bytes = static_cast<UINT>(source->size());
        open_.push_back(std::move(source));
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }

private:
    std::vector<std::unique_ptr<std::string>> open_;
};

std::string CacheKey(std::string_view name, const char* entryPoint) {
    return std::string(name) + "::" + entryPoint;
}

#ifdef LIQUIDOCK_SHADER_DIR
unsigned long long DirectoryStamp() {
    namespace fs = std::filesystem;
    unsigned long long stamp = 1469598103934665603ULL; // FNV-1a offset basis
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(LIQUIDOCK_SHADER_DIR, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto written = entry.last_write_time(ec).time_since_epoch().count();
        for (const char c : entry.path().filename().string()) {
            stamp = (stamp ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        }
        stamp = (stamp ^ static_cast<unsigned long long>(written)) * 1099511628211ULL;
    }
    return stamp;
}
#endif

} // namespace

std::string ShaderCache::LoadSource(std::string_view filename) {
#ifdef LIQUIDOCK_SHADER_DIR
    const std::filesystem::path path = std::filesystem::path(LIQUIDOCK_SHADER_DIR) / filename;
    if (std::filesystem::exists(path)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) {
            std::string text(static_cast<size_t>(size), '\0');
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.wstring().c_str(), L"rb") == 0 && file) {
                const size_t read = fread(text.data(), 1, text.size(), file);
                fclose(file);
                text.resize(read);
                return text;
            }
        }
    }
#endif
    const std::string_view embedded = shaders::Find(filename);
    return std::string(embedded);
}

ComPtr<ID3DBlob> ShaderCache::Compile(std::string_view name, const char* entryPoint,
                                      const char* profile) {
    const std::string filename = std::string(name) + ".hlsl";
    const std::string source = LoadSource(filename);
    if (source.empty()) {
        LogError("Shader source not found: {}", filename);
        return nullptr;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef LIQUIDOCK_DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    IncludeHandler includes;
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), filename.c_str(), nullptr,
                                  &includes, entryPoint, profile, flags, 0, &bytecode, &errors);

    if (errors && errors->GetBufferSize() > 0) {
        LogError("{} [{}]: {}", filename, entryPoint,
                 static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (FAILED(hr)) {
        return nullptr;
    }
    return bytecode;
}

ComPtr<ID3D11VertexShader> ShaderCache::VertexShader(std::string_view name,
                                                     const char* entryPoint) {
    const std::string key = CacheKey(name, entryPoint);
    if (const auto it = vertexShaders_.find(key); it != vertexShaders_.end()) {
        return it->second;
    }

    ComPtr<ID3DBlob> bytecode = Compile(name, entryPoint, "vs_5_0");
    if (!bytecode) {
        return nullptr;
    }

    ComPtr<ID3D11VertexShader> shader;
    const HRESULT hr = device_->CreateVertexShader(bytecode->GetBufferPointer(),
                                                   bytecode->GetBufferSize(), nullptr, &shader);
    if (FAILED(hr)) {
        LogError("CreateVertexShader({}::{}) failed - {}", name, entryPoint, FormatHResult(hr));
        return nullptr;
    }
    vertexShaders_[key] = shader;
    return shader;
}

ComPtr<ID3D11PixelShader> ShaderCache::PixelShader(std::string_view name, const char* entryPoint) {
    const std::string key = CacheKey(name, entryPoint);
    if (const auto it = pixelShaders_.find(key); it != pixelShaders_.end()) {
        return it->second;
    }

    ComPtr<ID3DBlob> bytecode = Compile(name, entryPoint, "ps_5_0");
    if (!bytecode) {
        return nullptr;
    }

    ComPtr<ID3D11PixelShader> shader;
    const HRESULT hr = device_->CreatePixelShader(bytecode->GetBufferPointer(),
                                                  bytecode->GetBufferSize(), nullptr, &shader);
    if (FAILED(hr)) {
        LogError("CreatePixelShader({}::{}) failed - {}", name, entryPoint, FormatHResult(hr));
        return nullptr;
    }
    pixelShaders_[key] = shader;
    return shader;
}

bool ShaderCache::PollForChanges() {
#ifdef LIQUIDOCK_SHADER_DIR
    const unsigned long long stamp = DirectoryStamp();
    if (stamp == sourceStamp_) {
        return false;
    }
    const bool firstPoll = (sourceStamp_ == 0);
    sourceStamp_ = stamp;
    if (firstPoll) {
        return false;
    }

    // A failed recompile leaves the caller with nothing to draw for one frame,
    // which is the right trade: the error lands in the log immediately and the
    // next save fixes it.
    vertexShaders_.clear();
    pixelShaders_.clear();
    LogInfo("Shader sources changed, recompiling");
    return true;
#else
    return false;
#endif
}

} // namespace liquidock
