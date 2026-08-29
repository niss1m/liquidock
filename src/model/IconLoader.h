#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model/DockItem.h"

namespace liquidock {

// One extracted icon, as premultiplied BGRA at `size` x `size`.
struct IconBitmap {
    int slot = -1; // which item asked for it
    int size = 0;
    std::vector<uint32_t> pixels;
    // The executable this item ultimately launches, for the running indicator.
    // Resolved here because it is the same question the icon asks - "what does
    // this shortcut actually point at" - answered on the same thread, against
    // the same possibly-slow shell.
    std::wstring target;
};

// Pulls icons out of the shell, off the UI thread.
//
// Icon extraction is the slowest thing the dock does at startup: a cold
// IShellItemImageFactory call can take tens of milliseconds while the shell
// faults in the owning module, and a dozen of those is a visible stall. So it
// happens on a worker thread, which posts a message to the dock as each icon
// lands. The dock draws whatever it has - the bar appears immediately and fills
// in over the next fraction of a second, rather than the window appearing late.
//
// The worker only ever produces CPU-side pixels. Everything that touches the
// D3D device stays on the render thread, so there is no device context to
// serialise and no multithread flag to pay for.
class IconLoader {
public:
    IconLoader() = default;
    IconLoader(const IconLoader&) = delete;
    IconLoader& operator=(const IconLoader&) = delete;
    ~IconLoader();

    // Starts extracting `paths` at `size` pixels square. `notify` is posted
    // WM_APP-style each time results are ready to collect. Cancels and replaces
    // any load already in flight.
    void Start(std::vector<DockItem> items, int size, HWND notify, UINT message);

    // Signals the worker to stop and waits for it. Safe to call twice.
    void Stop();

    // Moves everything finished since the last call into `out`.
    void Collect(std::vector<IconBitmap>& out);

private:
    void Run(std::vector<DockItem> items, int size, HWND notify, UINT message);

    std::thread worker_;
    std::mutex mutex_;
    std::vector<IconBitmap> ready_;
    // The generation counter is what makes cancellation cheap: a worker started
    // for an older generation notices on its next iteration and drops its
    // results instead of racing the new one into the queue.
    std::atomic<unsigned> generation_{0};
};

} // namespace liquidock
