#include "model/IconLoader.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "core/Log.h"
#include "model/ItemStore.h"
#include "model/SystemItems.h"

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

    // Straightened and premultiplied in place, before any of it is averaged:
    // mixing straight-alpha pixels together darkens the transparent edge, which
    // is the halo this was fixing in the first place.
    for (uint32_t& pixel : source) {
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
        pixel = (a << 24) | (r << 16) | (g << 8) | b;
    }

    out.assign(static_cast<size_t>(size) * size, 0);

    // SIIGBF_BIGGERSIZEOK means what comes back is routinely larger than what
    // was asked for - forty-eight for a forty, two hundred and fifty-six for
    // anything - and it used to be *cropped* to the requested square, which
    // quietly threw away the right and bottom of every oversized icon. Scaled
    // down instead, by averaging every source pixel that falls inside a
    // destination one: at these ratios that is a better answer than anything
    // sampling-based, and it costs one pass over an image measured in tens of
    // kilobytes.
    const int sourceWidth = info.bmWidth;
    const int sourceHeight = info.bmHeight;
    const float ratio =
        std::min(static_cast<float>(size) / sourceWidth, static_cast<float>(size) / sourceHeight);

    if (ratio >= 1.0f) {
        // Small enough to place as it is. Centred rather than stretched: a
        // thirty-two pixel icon blown up to forty is worse than one sitting in
        // the middle of forty with room around it.
        const int width = std::min(sourceWidth, size);
        const int height = std::min(sourceHeight, size);
        const int left = (size - width) / 2;
        const int top = (size - height) / 2;
        for (int y = 0; y < height; ++y) {
            const uint32_t* sourceRow = source.data() + static_cast<size_t>(y) * sourceWidth;
            uint32_t* destRow = out.data() + static_cast<size_t>(top + y) * size + left;
            std::copy(sourceRow, sourceRow + width, destRow);
        }
        return true;
    }

    const int width = std::max(1, static_cast<int>(std::lround(sourceWidth * ratio)));
    const int height = std::max(1, static_cast<int>(std::lround(sourceHeight * ratio)));
    const int left = (size - width) / 2;
    const int top = (size - height) / 2;

    for (int y = 0; y < height; ++y) {
        const int y0 = y * sourceHeight / height;
        const int y1 = std::max(y0 + 1, (y + 1) * sourceHeight / height);
        uint32_t* destRow = out.data() + static_cast<size_t>(top + y) * size + left;
        for (int x = 0; x < width; ++x) {
            const int x0 = x * sourceWidth / width;
            const int x1 = std::max(x0 + 1, (x + 1) * sourceWidth / width);
            uint32_t sums[4]{};
            for (int sy = y0; sy < y1; ++sy) {
                const uint32_t* sourceRow = source.data() + static_cast<size_t>(sy) * sourceWidth;
                for (int sx = x0; sx < x1; ++sx) {
                    const uint32_t pixel = sourceRow[sx];
                    sums[0] += pixel & 0xFF;
                    sums[1] += (pixel >> 8) & 0xFF;
                    sums[2] += (pixel >> 16) & 0xFF;
                    sums[3] += pixel >> 24;
                }
            }
            const uint32_t count = static_cast<uint32_t>(y1 - y0) * static_cast<uint32_t>(x1 - x0);
            const uint32_t half = count / 2;
            destRow[x] = (((sums[3] + half) / count) << 24) | (((sums[2] + half) / count) << 16) |
                         (((sums[1] + half) / count) << 8) | ((sums[0] + half) / count);
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

// Decodes an image file into a premultiplied BGRA square, letterboxed so a
// non-square source keeps its proportions rather than being stretched.
//
// This is what makes a themed icon set usable: people who care about how their
// dock looks have a folder of PNGs, and pointing an entry at one has to be as
// ordinary as pointing it at an executable.
bool LoadIconImage(const std::wstring& path, int size, std::vector<uint32_t>& out) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) || sourceWidth == 0 ||
        sourceHeight == 0) {
        return false;
    }

    // Fit inside the cell, preserving the aspect ratio.
    const double aspect = static_cast<double>(sourceWidth) / sourceHeight;
    UINT targetWidth = static_cast<UINT>(size);
    UINT targetHeight = static_cast<UINT>(size);
    if (aspect > 1.0) {
        targetHeight = std::max<UINT>(1, static_cast<UINT>(size / aspect));
    } else if (aspect < 1.0) {
        targetWidth = std::max<UINT>(1, static_cast<UINT>(size * aspect));
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), targetWidth, targetHeight,
                                  WICBitmapInterpolationModeHighQualityCubic))) {
        return false;
    }

    // PBGRA: premultiplied, which is what the atlas and the swap chain both
    // want, so WIC does the multiply rather than a loop here doing it wrong.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    std::vector<uint32_t> scaled(static_cast<size_t>(targetWidth) * targetHeight);
    if (FAILED(converter->CopyPixels(nullptr, targetWidth * 4,
                                     static_cast<UINT>(scaled.size() * 4),
                                     reinterpret_cast<BYTE*>(scaled.data())))) {
        return false;
    }

    out.assign(static_cast<size_t>(size) * size, 0);
    const int left = (size - static_cast<int>(targetWidth)) / 2;
    const int top = (size - static_cast<int>(targetHeight)) / 2;
    for (UINT y = 0; y < targetHeight; ++y) {
        std::copy_n(scaled.begin() + static_cast<size_t>(y) * targetWidth, targetWidth,
                    out.begin() + static_cast<size_t>(top + y) * size + left);
    }
    return true;
}

// One of the shell's own icons, by name rather than by asking a path what it
// looks like.
//
// This is what makes a live icon possible at all. Asking the shell for the
// Recycle Bin's icon returns whatever it has cached against that parsing name,
// which is the whole reason the bin appears to stay empty; naming the full one
// and the empty one as the two distinct stock icons they are leaves nothing to
// cache. It is also the only way to put an icon on Show Desktop and Lock, which
// have no path to ask about.
//
// The location is taken rather than the handle - SHGSI_ICONLOCATION gives back
// the file and index the shell would have used - so the icon can be extracted
// at the size the atlas wants instead of at whatever the system icon metric
// happens to be, which on a high-DPI dock is a visibly soft thirty-two pixels.
bool LoadStockIcon(int stock, int size, std::vector<uint32_t>& out) {
    if (stock < 0) {
        return false;
    }
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(stock), SHGSI_ICONLOCATION,
                                  &info))) {
        return false;
    }

    HICON handle = nullptr;
    if (FAILED(SHDefExtractIconW(info.szPath, info.iIcon, 0, &handle, nullptr,
                                 static_cast<UINT>(size))) ||
        !handle) {
        return false;
    }

    ICONINFO parts{};
    bool copied = false;
    if (GetIconInfo(handle, &parts)) {
        copied = parts.hbmColor && CopyBitmap(parts.hbmColor, size, out);
        if (parts.hbmColor) {
            DeleteObject(parts.hbmColor);
        }
        if (parts.hbmMask) {
            DeleteObject(parts.hbmMask);
        }
    }
    DestroyIcon(handle);
    return copied;
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

void IconLoader::Start(std::vector<DockItem> items, int size, HWND notify, UINT message,
                       std::vector<int> slots) {
    Stop();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ready_.clear();
    }
    worker_ = std::thread([this, items = std::move(items), size, notify, message,
                           slots = std::move(slots)]() mutable {
        // Shell extensions are overwhelmingly apartment-threaded, and several
        // of them will refuse to load at all on an MTA thread.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            return;
        }
        Run(std::move(items), size, notify, message, std::move(slots));
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

void IconLoader::Run(std::vector<DockItem> items, int size, HWND notify, UINT message,
                     std::vector<int> slots) {
    const unsigned generation = generation_.load(std::memory_order_relaxed);

    for (size_t index = 0; index < items.size(); ++index) {
        if (generation_.load(std::memory_order_relaxed) != generation) {
            return; // cancelled; a newer load owns the queue now
        }

        // A divider has no target to ask the shell about, and asking anyway
        // means a failed shell call per separator on every reload.
        if (items[index].kind == ItemKind::Separator) {
            continue;
        }

        IconBitmap icon;
        icon.slot = (index < slots.size()) ? slots[index] : static_cast<int>(index);
        icon.size = size;

        // The dock's own entry has no path to ask about, so it falls back to
        // the running executable - which carries the application icon, and is
        // the right answer even when nobody has pointed it at a PNG.
        std::wstring expanded = ItemStore::ExpandPath(items[index].path);
        if (items[index].kind == ItemKind::Settings && expanded.empty()) {
            wchar_t self[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self))) > 0) {
                expanded = self;
            }
        }
        icon.target = ResolveExecutable(expanded);

        // An explicit image wins over whatever the shell would say. If it fails
        // to decode - a path that has moved, a file that is not an image - the
        // shell icon is still a better answer than a placeholder.
        // `icon = <file>` means "take the icon from this file", whatever kind of
        // file it is. Usually that is a PNG from a themed set, so the image
        // decoder is tried first and is the best answer when it works. But
        // pointing at an executable, a .ico or a .dll is just as reasonable a
        // thing to want - and it is what an imported Nexus dock actually
        // contains - so the shell gets a turn before giving up on the override.
        // A built-in Windows entry may have an icon that depends on the state
        // of the machine rather than on anything in the config file. Asking now
        // is what makes it live: this runs again whenever the shell says the
        // thing behind it moved.
        const SystemEntry* system = FindSystemEntry(items[index].systemId);
        const int stateIcon = system ? SystemStateIcon(*system) : -1;

        // Which override applies. A live entry has two, and the second one only
        // wins when it is actually set: someone who chose a single image meant
        // it for the entry, not for half of it.
        std::wstring custom = items[index].iconPath;
        if (stateIcon == SIID_RECYCLERFULL && !items[index].iconAltPath.empty()) {
            custom = items[index].iconAltPath;
        }
        custom = ItemStore::ExpandPath(custom);

        bool loaded = false;
        if (!custom.empty()) {
            loaded = LoadIconImage(custom, size, icon.pixels) ||
                     ExtractIcon(custom, size, icon.pixels);
            if (!loaded) {
                LogWarn("Could not read the icon for dock item {}; using the target's own",
                        index);
            }
        }
        // The state icon comes *before* the shell's answer for the path, which
        // is the entire trick: the shell caches one icon per parsing name, so
        // asking it about the bin returns whichever state it saw first.
        if (!loaded && stateIcon >= 0) {
            loaded = LoadStockIcon(stateIcon, size, icon.pixels);
        }
        if (!loaded && !expanded.empty()) {
            loaded = ExtractIcon(expanded, size, icon.pixels);
        }
        // And last, the entry's own stock icon, which is all Show Desktop and
        // Lock have - there is no file behind either of them to ask.
        if (!loaded && system) {
            loaded = LoadStockIcon(system->stockIcon, size, icon.pixels);
        }
        if (!loaded) {
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
