// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/hle/call_adapter.h"

namespace Frontend {
class Window;
}
namespace Vulkan {
struct Driver;
}

namespace Core::HostRuntime {
class GuestSaveDialog;
// One production Linker/MemoryManager/thread domain per Session generation.
// CPU implementation is injected; this library never links a second FEX runtime.
class GuestRuntime final {
public:
    static constexpr std::uint64_t ReservationBegin = 0x400000;
    static constexpr std::uint64_t ReservationEnd = 0x1e00000000ULL; // 120 GiB
    GuestRuntime(GuestCpu::CpuContext& cpu, GuestCpu::GuestAddressSpace& space,
                 GuestCpu::Hle::HleCallRegistry& registry);
    ~GuestRuntime();
    GuestRuntime(const GuestRuntime&) = delete;
    GuestRuntime& operator=(const GuestRuntime&) = delete;
    void ConfigureSaveDialog(std::shared_ptr<GuestSaveDialog> dialog);
    void ConfigureGraphics(std::shared_ptr<Frontend::Window> window,
                           std::shared_ptr<const Vulkan::Driver> driver);
    void Prepare(const std::filesystem::path& executable,
                 const std::vector<std::filesystem::path>& modules = {});
    GuestCpu::Result<GuestCpu::GuestCallResult> Run(const std::vector<std::string>& args);
    GuestCpu::Status RequestCancel();
    GuestCpu::Status WaitStopped(std::uint64_t timeout_ns);
    std::string Diagnostics() const;
    std::string OperationName(std::uint64_t operation) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
