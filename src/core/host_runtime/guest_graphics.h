// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mutex>
#include <string_view>
#include <functional>
#include <memory>
#include "common/types.h"

namespace Frontend {
class Window;
}
namespace Vulkan {
struct Driver;
}
namespace Libraries::Kernel {
class EqueueInternal;
}
namespace Libraries::VideoOut {
class VideoOutDriver;
}

namespace Core::HostRuntime {
// Owned by the production runtime, inside its exclusive desktop-service lease.
// GPU/VideoOut workers retire before the guest VM and its singleton bindings.
class GuestGraphics final {
public:
    GuestGraphics(std::shared_ptr<Frontend::Window> window,
                  std::shared_ptr<const Vulkan::Driver> driver, std::function<u64()> process_time,
                  std::function<u64()> tsc, std::function<bool()> splash_visible, u64 guest_labels,
                  std::function<void()> fault_notify);
    ~GuestGraphics();
    GuestGraphics(const GuestGraphics&) = delete;
    GuestGraphics& operator=(const GuestGraphics&) = delete;
    void RequestStop();
    void WaitIdle();
    void CheckHealth();
    s64 CreateEqueue(std::string_view name);
    s32 DeleteEqueue(s64 handle);
    Libraries::Kernel::EqueueInternal* FindEqueue(s64 handle);
    bool Stopping() const;
    std::recursive_mutex& SubmissionMutex();
    Libraries::VideoOut::VideoOutDriver& VideoOut();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
