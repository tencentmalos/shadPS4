// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Backend seam for the embeddable host session.
//
// The lifecycle core (SessionCore) talks only to ISessionBackend; it never calls
// CreateContext directly. That indirection is the whole point: a host unit test
// injects a deterministic FakeBackend (no FEX, no JIT), while the real Android
// .so injects the FEX adapter. Nothing here references a backend type, so this
// header links against guest_cpu_api alone.
//
// SessionRuntime is an OPAQUE, per-session bundle of backend resources. SessionCore
// holds a std::shared_ptr<SessionRuntime> and never inspects its internals. The
// shared_ptr is the memory-safety half of the lifetime model: while a non-owner
// stopper holds a copy, the object (and the CpuContext inside a real runtime)
// cannot be freed, so a Stop that races the owner's teardown dereferences a live
// object rather than a freed one. The control-lease counter in SessionCore is the
// ordering half: the owner performs Destroy only after every in-flight backend
// call has returned.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef SESSION_TEST_HOOKS
#include <atomic>
#endif

#include "core/guest_cpu/api/result.h"
#include "core/guest_cpu/api/status.h"

namespace Frontend {
class Window;
}
namespace Vulkan {
struct Driver;
}

namespace Core::HostRuntime {
class GuestSaveDialog;

// Opaque per-session backend resources. Concrete subclasses live in the backend
// (FexSessionRuntime holds GuestAddressSpace/CpuContext/ThreadHandle; the test
// FakeRuntime holds latches). SessionCore only ever moves a shared_ptr to this.
struct SessionRuntime {
    virtual ~SessionRuntime() = default;

#ifdef SESSION_TEST_HOOKS
    // Deterministic use-after-free tripwire: Destroy() sets this false and every
    // backend method asserts it is true on entry, so a call racing teardown is a
    // hard test failure without relying on ASan.
    std::atomic<bool> alive{true};
#endif
};

// Backend-neutral outcome of one Run. A guest fault is an outcome, not an
// exception: the assessment's core defect was mapping "non-Cancel" to exit 0, so
// every abnormal end has its own value here and is surfaced, never swallowed.
enum class RunOutcome : std::uint32_t {
    Returned = 0,   // guest reached the return gate cleanly
    Cancelled,      // interrupted by RequestCancel
    Faulted,        // guest fault (GuestFault)
    BackendFailed,  // backend reported an internal failure
    Unsupported,    // requested a feature the backend does not implement
    Unexpected,     // a stop reason not expected for this session kind
    StartFailed,    // Prepare or owner-thread creation failed
};

[[nodiscard]] const char* ToString(RunOutcome outcome) noexcept;

struct RunReport final {
    RunOutcome outcome{RunOutcome::Returned};
    // guest_cpu ErrorCategory ordinal when applicable, else 0 (None).
    std::uint32_t error_category{0};
    // Short structured note (e.g. "guest_rip=0x...", a Describe() string). Not a
    // user-facing message.
    std::string detail;
};

struct SessionParams final {
    std::string content_id;       // game id / "smoke"; diagnostic only
    std::uint64_t iterations{0};  // smoke-loop bound; 0 = backend default
    // Nonempty selects the production Linker/VM/runtime. CPU smoke remains an
    // explicit separate mode. The caller supplies the installed effective path.
    std::string executable_path;
    std::vector<std::string> module_paths;
    std::function<std::shared_ptr<GuestSaveDialog>(std::uint64_t)> create_save_dialog;
    bool requires_platform_ready{};
    std::uint64_t generation{}; // Minted by SessionCore, never supplied by JNI.
    std::function<std::shared_ptr<Frontend::Window>(std::uint64_t)> create_window;
    std::function<std::shared_ptr<const Vulkan::Driver>()> load_graphics_driver;
};

// Opaque cancel handle. RequestCancel returns one; the caller passes it to
// WaitStopped. The backend interprets `value` (the FEX adapter uses it to find
// the guest_cpu InterruptTicket it stored for this request, so concurrent
// stoppers do not clobber each other).
struct StopTicket final {
    std::uint64_t value{0};
    bool valid{false};
};

class ISessionBackend {
public:
    virtual ~ISessionBackend() = default;

    ISessionBackend(const ISessionBackend&) = delete;
    ISessionBackend& operator=(const ISessionBackend&) = delete;

    // Owner thread. Creates the address space, context and guest thread. On
    // failure returns an error and no runtime is published.
    [[nodiscard]] virtual Core::GuestCpu::Result<std::shared_ptr<SessionRuntime>> Prepare(
        const SessionParams& params) = 0;

    // Owner thread. Runs until a boundary; blocking. Always returns a report.
    [[nodiscard]] virtual RunReport Run(SessionRuntime& runtime) = 0;

    // Any thread. Requests cancellation of a running session.
    [[nodiscard]] virtual Core::GuestCpu::Result<StopTicket> RequestCancel(
        SessionRuntime& runtime) = 0;

    // Any thread. Bounded wait for the cancel to take effect. Never destroys the
    // thread; a timeout returns Timeout and leaves the request pending.
    [[nodiscard]] virtual Core::GuestCpu::Status WaitStopped(SessionRuntime& runtime,
                                                             const StopTicket& ticket,
                                                             std::uint64_t timeout_ns) = 0;

    // Owner thread. Destroys thread/context/space. Called exactly once, after the
    // control-lease has fully drained, so no other thread is inside a backend
    // call on this runtime.
    virtual void Destroy(SessionRuntime& runtime) = 0;

protected:
    ISessionBackend() = default;
};

}  // namespace Core::HostRuntime
