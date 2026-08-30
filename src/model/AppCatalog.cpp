#include "model/AppCatalog.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

#include "core/Log.h"
#include "gfx/GraphicsDevice.h" // ComPtr

namespace liquidock {
namespace {

// The AppsFolder's own parsing name. Passing the GUID rather than the friendly
// "shell:AppsFolder" because the friendly form goes through a different code
// path in some shells and is not guaranteed to resolve on all of them.
constexpr wchar_t kAppsFolder[] = L"shell:::{4234D49B-0245-4DF3-B780-3893943456E1}";

// Things the shell lists that nobody wants on a dock.
bool Uninteresting(const std::wstring& label) {
    static const wchar_t* const kSkip[] = {
        L"Uninstall", L"Read Me", L"Readme", L"Release Notes", L"License",
        L"Documentation", L"Help", L"Website", L"Support", L"Repair",
    };
    for (const wchar_t* skip : kSkip) {
        if (label.find(skip) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

AppCatalog::~AppCatalog() {
    Stop();
}

void AppCatalog::Stop() {
    stop_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AppCatalog::Start(HWND notify, UINT message) {
    Stop();
    stop_ = false;
    ready_ = false;
    worker_ = std::thread(&AppCatalog::Run, this, notify, message);
}

std::vector<CatalogEntry> AppCatalog::Take() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

void AppCatalog::Run(HWND notify, UINT message) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return;
    }

    std::vector<CatalogEntry> found;
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(kAppsFolder, nullptr, IID_PPV_ARGS(&folder)))) {
        ComPtr<IEnumShellItems> items;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
            ComPtr<IShellItem> item;
            while (!stop_.load(std::memory_order_relaxed) &&
                   items->Next(1, &item, nullptr) == S_OK) {
                PWSTR display = nullptr;
                PWSTR parsing = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing))) {
                    // What comes back for a child of this folder is a bare
                    // AUMID - "Anysphere.Cursor", "308046B0AF4A39CB" - not a
                    // path, and on its own it parses as nothing: it only means
                    // anything relative to the folder it came out of. Handing
                    // it back with the folder still attached is what makes it
                    // both launchable and askable for an icon. Win32 entries do
                    // come back as real paths, and those are left alone.
                    std::wstring path = parsing;
                    if (path.find(L'\\') == std::wstring::npos) {
                        path = L"shell:AppsFolder\\" + path;
                    }
                    CatalogEntry entry{display, std::move(path)};
                    if (!entry.label.empty() && !Uninteresting(entry.label)) {
                        found.push_back(std::move(entry));
                    }
                }
                CoTaskMemFree(display);
                CoTaskMemFree(parsing);
                item.Reset();
            }
        }
    }

    std::sort(found.begin(), found.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_ = std::move(found);
    }
    ready_ = true;
    LogInfo("App catalog: {} installed apps", entries_.size());
    if (notify && !stop_.load(std::memory_order_relaxed)) {
        PostMessageW(notify, message, 0, 0);
    }
    CoUninitialize();
}

} // namespace liquidock
