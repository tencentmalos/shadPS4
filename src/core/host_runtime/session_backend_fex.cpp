// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SessionCore owns lifecycle/generation; this backend owns the FEX context and
// production GuestRuntime (Module/Linker/VM/thread/TLS/HLE). An empty executable
// remains the explicitly selected CPU smoke path, never a fallback for game failure.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"
#include "core/guest_cpu/api/status.h"
#include "core/host_runtime/session_backend_fex.h"
#if defined(SHADPS4_TYPED_HLE_HOST)
#include "core/guest_cpu/fex/fex_context.h"
#include "core/host_runtime/guest_runtime.h"
#endif

namespace Core::HostRuntime {

using namespace Core::GuestCpu;

namespace {

void RuntimeStage(const char* stage) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "ProductionRuntime", "%s", stage);
#endif
}
std::atomic<std::uint64_t> next_stop_ticket{1};
constexpr std::uint64_t kReservationSize = std::uint64_t{1} << 28;
constexpr std::uint64_t kMappingSize = 0x4000;
constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;

// Bounded decrement loop; rdi = iteration count. Ends by jumping to the backend
// return gate (StopReason::Returned). Large-but-finite so a Cancel can interrupt
// it while a natural return is still reachable.
std::vector<std::uint8_t> BuildLoopRoutine(std::uint64_t gate_address) {
    std::vector<std::uint8_t> code;
    auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes)
            code.push_back(b);
    };
    emit({0x48, 0x83, 0xef, 0x01}); // sub rdi, 1
    emit({0x75, 0xfa});             // jne -6
    emit({0x49, 0xbf});             // movabs r15, imm64
    for (int i = 0; i < 8; ++i)
        code.push_back(static_cast<std::uint8_t>((gate_address >> (8 * i)) & 0xff));
    emit({0x41, 0xff, 0xe7}); // jmp r15
    return code;
}

// The FEX-backed per-session runtime: the guest_cpu resources SessionCore holds
// as an opaque shared_ptr<SessionRuntime>.
struct FexSessionRuntime final : SessionRuntime {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    ThreadHandle thread{};
#if defined(SHADPS4_TYPED_HLE_HOST)
    std::unique_ptr<GuestRuntime> production;
    std::vector<std::string> args;
#endif

    // RequestCancel stores the ticket keyed by a monotonic StopTicket value, so
    // concurrent stoppers do not clobber each other's ticket. WaitStopped looks
    // it up. Guarded by its own mutex; independent of SessionCore's lock.
    std::mutex ticket_mtx;
    std::unordered_set<std::uint64_t> production_tickets;
    std::unordered_map<std::uint64_t, InterruptTicket> tickets;
};

} // namespace

Result<std::shared_ptr<SessionRuntime>> FexSessionBackend::Prepare(
    const SessionParams& params) try {
    auto rt = std::make_shared<FexSessionRuntime>();

    AddressSpaceConfig cfg{};
    cfg.reservation_size = kReservationSize;
    cfg.max_address = QueryBackendCapabilities().max_guest_address;
#if defined(SHADPS4_TYPED_HLE_HOST)
    if (!params.executable_path.empty()) {
        cfg.preferred_base = GuestRuntime::ReservationBegin;
        cfg.reservation_size = GuestRuntime::ReservationEnd - GuestRuntime::ReservationBegin;
        cfg.owned_ranges = {
            {GuestAddress{GuestRuntime::ReservationBegin},
             0x10000000 - GuestRuntime::ReservationBegin},
            {GuestAddress{0x100000000ULL}, GuestRuntime::ReservationEnd - 0x100000000ULL}};
    }
#else
    if (!params.executable_path.empty())
        return MakeError(ErrorCategory::Unsupported, "Prepare",
                         "production host library is required");
#endif
    RuntimeStage("prepare: reserve guest VA");
    auto space_r = GuestAddressSpace::Create(cfg);
    if (!space_r)
        return space_r.GetError();
    rt->space = std::move(space_r).Value();
#if defined(SHADPS4_TYPED_HLE_HOST)
    if (!params.executable_path.empty()) {
        RuntimeStage("prepare: create FEX context");
        CpuConfig cpu_config;
        cpu_config.resume_internal_drains = true;
        auto context = CreateContext(cpu_config, *rt->space);
        if (!context)
            return context.GetError();
        rt->context = std::move(context).Value();
        auto* registry =
            static_cast<Hle::HleCallRegistry*>(Fex::FexHleRegistryPointer(*rt->context));
        RuntimeStage("prepare: create production VM/Linker");
        rt->production = std::make_unique<GuestRuntime>(*rt->context, *rt->space, *registry);
        if (params.create_save_dialog)
            rt->production->ConfigureSaveDialog(params.create_save_dialog(params.generation));
        if (params.create_window) {
            if (!params.load_graphics_driver)
                throw std::runtime_error("Rendered session requires a native driver provider");
            rt->production->ConfigureGraphics(params.create_window(params.generation),
                                              params.load_graphics_driver());
        }
        std::vector<std::filesystem::path> modules;
        for (const auto& path : params.module_paths)
            modules.emplace_back(path);
        RuntimeStage("prepare: load/relocate modules");
        rt->production->Prepare(params.executable_path, modules);
        RuntimeStage("prepare: ready");
        rt->args = {params.executable_path};
        return std::shared_ptr<SessionRuntime>(std::move(rt));
    }
#endif

    const std::uint64_t base = rt->space->ReservationBase().value;
    const std::uint64_t code_base = base + kCodeOffset;
    const std::uint64_t stack_top = base + kStackOffset + kMappingSize - 16;

    if (auto m = rt->space->Map(GuestRange{GuestAddress{code_base}, kMappingSize},
                                GuestPermission::Read | GuestPermission::Write);
        !m)
        return m.GetError();
    if (auto m =
            rt->space->Map(GuestRange{GuestAddress{stack_top - kMappingSize + 16}, kMappingSize},
                           GuestPermission::Read | GuestPermission::Write);
        !m)
        return m.GetError();

    auto ctx_r = CreateContext(CpuConfig{}, *rt->space);
    if (!ctx_r)
        return ctx_r.GetError();
    rt->context = std::move(ctx_r).Value();

    const std::uint64_t gate = rt->context->Capabilities().return_gate_address;
    if (gate == 0)
        return MakeError(ErrorCategory::BackendFailure, "FexSessionBackend::Prepare",
                         "backend reported no return gate");

    auto code = BuildLoopRoutine(gate);
    if (auto w = rt->space->Write(GuestAddress{code_base},
                                  {reinterpret_cast<const std::byte*>(code.data()), code.size()});
        !w)
        return w.GetError();
    if (auto p = rt->space->Protect(GuestRange{GuestAddress{code_base}, kMappingSize},
                                    GuestPermission::Read | GuestPermission::Execute);
        !p)
        return p.GetError();

    const std::uint64_t iters =
        params.iterations != 0 ? params.iterations : (std::uint64_t{1} << 32);
    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{code_base};
    init.initial_rsp = GuestAddress{stack_top};
    init.guest_tid = 1;
    init.initial_state.fields = RegisterValidity::Gpr;
    init.initial_state.gpr_mask = (1u << Index(Gpr::Rdi));
    init.initial_state.values.Set(Gpr::Rdi, iters);

    auto th_r = rt->context->CreateThread(init);
    if (!th_r)
        return th_r.GetError();
    rt->thread = th_r.Value();

    return std::shared_ptr<SessionRuntime>(std::move(rt));
} catch (const std::bad_alloc&) {
    return MakeError(ErrorCategory::OutOfMemory, "FexSessionBackend::Prepare",
                     "production runtime allocation failed");
} catch (const std::exception& error) {
    return MakeError(ErrorCategory::BackendFailure, "FexSessionBackend::Prepare", error.what());
}

RunReport FexSessionBackend::Run(SessionRuntime& runtime) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
    RunReport report;
#if defined(SHADPS4_TYPED_HLE_HOST)
    if (rt.production) {
        RuntimeStage("run: enter production guest");
        auto run = rt.production->Run(rt.args);
        RuntimeStage("run: guest returned to host");
        if (!run) {
            report.outcome = RunOutcome::BackendFailed;
            report.error_category = static_cast<std::uint32_t>(run.GetError().category);
            report.detail = Describe(run.GetError());
        } else {
            report.outcome = run.Value().reason == StopReason::Returned    ? RunOutcome::Returned
                             : run.Value().reason == StopReason::Cancelled ? RunOutcome::Cancelled
                             : run.Value().reason == StopReason::GuestFault
                                 ? RunOutcome::Faulted
                                 : RunOutcome::BackendFailed;
            report.detail = "guest return=" + std::to_string(run.Value().return_value);
            if (const auto& fault = run.Value().fault) {
                if (fault->guest_rip)
                    report.detail += " guest_rip=" + std::to_string(*fault->guest_rip);
                if (fault->syscall_operation) {
                    report.detail += " operation=" + std::to_string(*fault->syscall_operation);
                    report.detail +=
                        " import=" + rt.production->OperationName(*fault->syscall_operation);
                }
                report.detail +=
                    " context=" + std::to_string(fault->context_id) +
                    " thread=" + std::to_string(run.Value().snapshot.thread_id) +
                    " generation=" + std::to_string(fault->thread_generation) +
                    " invocation=" + std::to_string(fault->invocation_id) +
                    " category=" + std::to_string(static_cast<unsigned>(fault->syscall_category));
                report.error_category = static_cast<std::uint32_t>(fault->syscall_category);
            }
        }
        report.detail += "\n" + rt.production->Diagnostics();
        return report;
    }
#endif
    auto run = rt.context->Run(rt.thread, RunOptions{});
    if (!run) {
        report.outcome = RunOutcome::BackendFailed;
        report.error_category = static_cast<std::uint32_t>(run.GetError().category);
        report.detail = Describe(run.GetError());
        return report;
    }
    const RunResult& r = run.Value();
    switch (r.primary_reason) {
    case StopReason::Returned:
        report.outcome = RunOutcome::Returned;
        break;
    case StopReason::Cancelled:
        report.outcome = RunOutcome::Cancelled;
        break;
    case StopReason::GuestFault: {
        report.outcome = RunOutcome::Faulted;
        std::string detail = "guest fault";
        if (r.fault && r.fault->guest_rip)
            detail += " guest_rip=0x" + [](std::uint64_t v) {
                static const char* h = "0123456789abcdef";
                std::string s;
                for (int i = 60; i >= 0; i -= 4)
                    s += h[(v >> i) & 0xf];
                return s;
            }(*r.fault->guest_rip);
        report.detail = std::move(detail);
        break;
    }
    case StopReason::BackendFailure:
        report.outcome = RunOutcome::BackendFailed;
        report.detail = "backend failure";
        break;
    case StopReason::Unsupported:
        report.outcome = RunOutcome::Unsupported;
        report.detail = "unsupported";
        break;
    case StopReason::PauseRequested:
    case StopReason::StepComplete:
    case StopReason::HleBoundary:
    default:
        report.outcome = RunOutcome::Unexpected;
        report.detail = "unexpected stop reason for smoke session";
        break;
    }
    return report;
}

Result<StopTicket> FexSessionBackend::RequestCancel(SessionRuntime& runtime) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
#if defined(SHADPS4_TYPED_HLE_HOST)
    if (rt.production) {
        auto result = rt.production->RequestCancel();
        if (!result)
            return result.GetError();
        std::lock_guard lock{rt.ticket_mtx};
        const auto id = next_stop_ticket.fetch_add(1);
        rt.production_tickets.insert(id);
        return StopTicket{id, true};
    }
#endif
    auto ticket = rt.context->RequestInterrupt(rt.thread, InterruptReason::Cancel);
    if (!ticket)
        return ticket.GetError();
    std::lock_guard lock{rt.ticket_mtx};
    const std::uint64_t id = next_stop_ticket.fetch_add(1);
    rt.tickets.emplace(id, ticket.Value());
    return StopTicket{id, true};
}

Status FexSessionBackend::WaitStopped(SessionRuntime& runtime, const StopTicket& ticket,
                                      std::uint64_t timeout_ns) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
#if defined(SHADPS4_TYPED_HLE_HOST)
    if (rt.production) {
        {
            std::lock_guard lock{rt.ticket_mtx};
            if (!ticket.valid || !rt.production_tickets.contains(ticket.value))
                return MakeError(ErrorCategory::InvalidArgument, "WaitStopped",
                                 "unknown generation stop ticket");
        }
        return rt.production->WaitStopped(timeout_ns);
    }
#endif
    InterruptTicket it{};
    {
        std::lock_guard lock{rt.ticket_mtx};
        auto found = rt.tickets.find(ticket.value);
        if (found == rt.tickets.end())
            return MakeError(ErrorCategory::InvalidArgument, "FexSessionBackend::WaitStopped",
                             "unknown stop ticket");
        it = found->second;
    }
    auto receipt = rt.context->WaitStopped(it, timeout_ns);
    if (!receipt)
        return receipt.GetError();
    return Ok();
}

void FexSessionBackend::Destroy(SessionRuntime& runtime) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
#if defined(SHADPS4_TYPED_HLE_HOST)
    rt.production.reset();
#endif
    if (rt.context && rt.thread.IsValid())
        (void)rt.context->DestroyThread(rt.thread);
    rt.context.reset();
    rt.space.reset();
    rt.thread = ThreadHandle{};
    std::lock_guard lock{rt.ticket_mtx};
    rt.tickets.clear();
    rt.production_tickets.clear();
}

} // namespace Core::HostRuntime
