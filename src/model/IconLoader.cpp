#include "model/IconLoader.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "core/Log.h"
#include "model/ItemStore.h"

namespace liquidock {
namespace {

using Microsoft::WRL::ComPtr;

// Reads a 32-bit DIB out of an HBITMAP and centres it in a `size` x `size`
// buffer of premultiplied BGRA.
bool CopyBitmap(HBITMAP bitmap, int size, std::vector<uint32_t>& out) {
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0) {
        return false;
    }

    const int width = std::min<int>(info.bmWidth, size);
    const int height = std::min<int>(info.bmHeight, size);

    BITMAPINFO request{};
    request.bmiHeader.biSize = sizeof(request.bmiHeader);
    request.bmiHeader.biWidth = info.bmWidth;
    // Negative height asks for a top-down DIB, which matches the row order both
    // D3D and this buffer use. Without it every icon arrives upside down.
    request.bmiHeader.biHeight = -info.bmHeight;
    request.bmiHeader.biPlanes = 1;
    request.bmiHeader.biBitCount = 32;
    request.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> source(static_cast<size_t>(info.bmWidth) * info.bmHeight);
    HDC screen = GetDC(nullptr);
    const int copied = GetDIBits(screen, bitmap, 0, static_cast<UINT>(info.bmHeight),
                                 source.data(), &request, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (copied == 0) {
        return false;
    }

    // Two things the shell does not promise. Some sources hand back a 32-bit
    // bitmap whose alpha channel was never filled in, which would render as
    // nothing at all; and the alpha that is there may be straight rather than
    // premultiplied, which would render with a bright halo. Both are decidable
    // from the pixels: all-zero alpha means "no alpha channel", and a colour
    // channel exceeding alpha is impossible in a premultiplied image.
    bool anyAlpha = false;
    bool straightAlpha = false;
    for (const uint32_t pixel : source) {
        const uint32_t a = pixel >> 24;
        if (a != 0) {
            anyAlpha = true;
            if ((pixel & 0xFF) > a || ((pixel >> 8) & 0xFF) > a || ((pixel >> 16) & 0xFF) > a) {
                straightAlpha = true;
                break;
            }
        }
    }

    out.assign(static_cast<size_t>(size) * size, 0);
    const int left = (size - width) / 2;
    const int top = (size - height) / 2;

    for (int y = 0; y < height; ++y) {
        const uint32_t* sourceRow = source.data() + static_cast<size_t>(y) * info.bmWidth;
        uint32_t* destRow = out.data() + static_cast<size_t>(top + y) * size + left;
        for (int x = 0; x < width; ++x) {
            const uint32_t pixel = sourceRow[x];
            uint32_t a = pixel >> 24;
            uint32_t b = pixel & 0xFF;
            uint32_t g = (pixel >> 8) & 0xFF;
            uint32_t r = (pixel >> 16) & 0xFF;
            if (!anyAlpha) {
                a = 255;
            } else if (straightAlpha) {
                b = (b * a + 127) / 255;
                g = (g * a + 127) / 255;
                r = (r * a + 127) / 255;
            }
            destRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

// Drawn when the shell cannot produce an icon - a bad path in the config file,
// most often. A rounded outline reads as "this entry did not resolve", which is
// more useful than a gap in the dock that looks like a layout bug.
void DrawPlaceholder(int size, std::vector<uint32_t>& out) {
    out.assign(static_cast<size_t>(size) * size, 0);

    const float extent = size * 0.5f;
    const float half = extent * 0.72f;
    const float radius = half * 0.34f;
    const float stroke = std::max(1.5f, size * 0.045f);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float px = x + 0.5f - extent;
            const float py = y + 0.5f - extent;

            // The same rounded-box distance field the glass uses, evaluated on
            // the CPU because there is no geometry here worth a draw call.
            const float qx = std::fabs(px) - half + radius;
            const float qy = std::fabs(py) - half + radius;
            const float outside =
                std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                          std::max(qy, 0.0f) * std::max(qy, 0.0f));
            const float distance = std::min(std::max(qx, qy), 0.0f) + outside - radius;

            const float fill = std::clamp(0.5f - distance, 0.0f, 1.0f) * 0.16f;
            const float ring =
                std::clamp(0.5f - std::fabs(distance + stroke * 0.5f) + stroke * 0.5f, 0.0f, 1.0f) *
                0.45f;
            const float alpha = std::clamp(fill + ring, 0.0f, 1.0f);

            const auto channel = static_cast<uint32_t>(std::lround(alpha * 255.0f));
            out[static_cast<size_t>(y) * size + x] =
                (channel << 24) | (channel << 16) | (channel << 8) | channel;
        }
    }
}

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
    const size_t length = wcslen(suffix);
    return text.size() >= length &&
           _wcsicmp(text.c_str() + text.size() - length, suffix) == 0;
}

// What a dock item ultimately runs, for the running indicator to watch for.
// Only executables can be "running": a folder or the recycle bin has no answer,
// and saying so with an empty string is better than guessing.
std::wstring ResolveExecutable(const std::wstring& path) {
    if (EndsWith(path, L".exe")) {
        return path;
    }
    if (!EndsWith(path, L".lnk")) {
        return {};
    }

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) {
        return {};
    }
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) {
        return {};
    }

    wchar_t target[MAX_PATH]{};
    // RAWPATH returns what the shortcut stores without trying to chase a moved
    // or offline target, which is what IShellLink::Resolve does - and that can
    // block for seconds on a network path.
    if (FAILED(link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH))) {
        return {};
    }
    const std::wstring expanded = ItemStore::ExpandPath(target);
    return EndsWith(expanded, L".exe") ? expanded : std::wstring{};
}

bool ExtractIcon(const std::wstring& path, int size, std::vector<uint32_t>& out) {
    ComPtr<IShellItemImageFactory> factory;
    // Parsing names cover every kind of entry the config file accepts - files,
    // folders, .lnk shortcuts, packaged apps under shell:AppsFolder, and the
    // ::{guid} virtual folders - which is why the item model can keep them all
    // in one string field.
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    HBITMAP bitmap = nullptr;
    // ICONONLY, not the thumbnail: a .lnk to a picture would otherwise come
    // back as the picture. BIGGERSIZEOK lets the shell hand over the next size
    // up rather than a blurry upscale, and CopyBitmap centres whatever arrives.
    const SIZE requested{size, size};
    if (FAILED(factory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) ||
        !bitmap) {
        return false;
    }

    const bool copied = CopyBitmap(bitmap, size, out);
    DeleteObject(bitmap);
    return copied;
}

} // namespace

IconLoader::~IconLoader() {
    Stop();
}

void IconLoader::Start(const std::vector<std::wstring>& paths, int size, HWND notify,
                       UINT message) {
    Stop();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ready_.clear();
    }
    worker_ = std::thread([this, paths, size, notify, message] {
        // Shell extensions are overwhelmingly apartment-threaded, and several
        // of them will refuse to load at all on an MTA thread.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            return;
        }
        Run(paths, size, notify, message);
        CoUninitialize();
    });
}

void IconLoader::Stop() {
    // Bumping the generation is what tells a worker still inside a slow shell
    // call to throw its remaining results away rather than push them into a
    // queue the next load already owns.
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void IconLoader::Collect(std::vector<IconBitmap>& out) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (ready_.empty()) {
        return;
    }
    out.insert(out.end(), std::make_move_iterator(ready_.begin()),
               std::make_move_iterator(ready_.end()));
    ready_.clear();
}

void IconLoader::Run(std::vector<std::wstring> paths, int size, HWND notify, UINT message) {
    const unsigned generation = generation_.load(std::memory_order_relaxed);

    for (size_t index = 0; index < paths.size(); ++index) {
        if (generation_.load(std::memory_order_relaxed) != generation) {
            return; // cancelled; a newer load owns the queue now
        }

        IconBitmap icon;
        icon.slot = static_cast<int>(index);
        icon.size = size;

        const std::wstring expanded = ItemStore::ExpandPath(paths[index]);
        icon.target = ResolveExecutable(expanded);
        if (!ExtractIcon(expanded, size, icon.pixels)) {
            LogWarn("No icon for dock item {}; drawing a placeholder", index);
            DrawPlaceholder(size, icon.pixels);
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (generation_.load(std::memory_order_relaxed) != generation) {
                return;
            }
            ready_.push_back(std::move(icon));
        }

        // Post per icon rather than once at the end. The dock is already on
        // screen by now, so this fills it in as the icons arrive instead of
        // showing a row of placeholders and then swapping them all at once.
        if (notify) {
            PostMessageW(notify, message, 0, 0);
        }
    }
}

} // namespace liquidock
