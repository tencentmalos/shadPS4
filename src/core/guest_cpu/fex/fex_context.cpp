// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/fex/entry_backedge_pass.h"
#include "core/guest_cpu/fex/fex_context.h"
#include "core/guest_cpu/api/access_fault.h"
#if defined(GUEST_CPU_TEST_HOOKS)
#include "core/guest_cpu/fex/test_run_gate.h"
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>

#include "Common/HostFeatures.h"
#include "Interface/Core/CPUBackend.h"
#include "Utils/Allocator.h" // Pinned FEX private allocator initialization; no VA-stealing hooks.
#include "core/guest_cpu/hle/call_adapter.h"
#include "core/guest_cpu/hle/scope.h"

namespace Core::GuestCpu::Fex {
namespace {

// One live context per process (API contract §2). This is a live-instance rule,
// not a once-ever latch: destroying the context must allow a rebuild, which
// acceptance L01 exercises 100 times.
std::atomic<bool> g_context_active{false};

// --- register mapping --------------------------------------------------------
// The public Gpr enum uses the x86-64 encoding order, which is also FEX's gregs
// index order. Assert it rather than trusting the coincidence: a divergence
// would silently swap registers.
static_assert(static_cast<int>(Gpr::Rax) == FEXCore::X86State::REG_RAX);
static_assert(static_cast<int>(Gpr::Rcx) == FEXCore::X86State::REG_RCX);
static_assert(static_cast<int>(Gpr::Rsp) == FEXCore::X86State::REG_RSP);
static_assert(static_cast<int>(Gpr::R15) == FEXCore::X86State::REG_R15);
static_assert(kGprCount == FEXCore::Core::CPUState::NUM_GPRS);

Error BackendError(ErrorCategory category, std::string_view operation, std::string detail,
                   int errno_value = 0) {
    Error error = MakeError(category, operation, std::move(detail));
    if (errno_value != 0) {
        error.system_error = errno_value;
    }
    return error;
}

// Map FEX's CPUState to the public register file and back at an HLE boundary. GPR order matches
// by construction (the static_asserts above); xmm low/high words are FEX's packed 128-bit view.
void RegistersFromCpuState(const FEXCore::Core::CPUState& state, RegisterFile& out) {
    for (std::size_t i = 0; i < kGprCount; ++i) {
        out.gpr[i] = state.gregs[i];
    }
    for (std::size_t i = 0; i < kXmmCount && i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
        out.xmm[i] = Xmm{state.xmm.sse.data[i][0], state.xmm.sse.data[i][1]};
    }
    out.rip = state.rip;
    out.mxcsr = state.mxcsr;
    out.fs_base = state.fs_cached;
    out.gs_base = state.gs_cached;
}
void ApplyRegistersToCpuState(const RegisterFile& in, FEXCore::Core::CPUState& state) {
    for (std::size_t i = 0; i < kGprCount; ++i) {
        state.gregs[i] = in.gpr[i];
    }
    for (std::size_t i = 0; i < kXmmCount && i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
        state.xmm.sse.data[i][0] = in.xmm[i].low;
        state.xmm.sse.data[i][1] = in.xmm[i].high;
    }
    state.rip = in.rip;
    state.mxcsr = in.mxcsr;
    state.fs_cached = in.fs_base;
    state.gs_cached = in.gs_base;
}

// Monotonic context identity. Tickets carry it so one issued by a destroyed context cannot be
// mistaken for a valid request against its replacement: thread ids and epochs both restart, this
// does not.
std::uint64_t NextContextId() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

// --- FEXCore embedder obligations -------------------------------------------
// InitCore dereferences the signal delegator to install the dispatcher config,
// so a context without one crashes there. FEXCore::SignalDelegator is concrete
// and only stores what the dispatcher hands it; shadPS4 installs its own signal
// handling, so this supplies the delegator object without taking over delivery.
class FexSignalDelegator final : public FEXCore::SignalDelegator {
  public:
    explicit FexSignalDelegator(std::uintptr_t callback_return)
        : callback_return_(callback_return) {}

    uintptr_t GetThunkCallbackRET() const override { return callback_return_; }

  private:
    std::uintptr_t callback_return_{};
};

// --- entry-boundary interrupts ------------------------------------------------
// See docs/validation/round2/g1-control-decision.md. The controller protects an
// owner-exclusive fault page; the JIT checks it before a basic block. We never
// redirect an arbitrary host PC out of a C++ call/lock or restore a suspended JIT
// stack after code publication. Pause returns Run to its owner like Cancel.
struct InterruptState final {
    // All fields are protected by FexCpuContext::lock_. No handler accesses this.
    std::map<std::uint64_t, InterruptReason> requests;
    std::uint32_t pending{};
    std::uint64_t request_epoch{};
    std::uint64_t acked_epoch{};
    std::optional<StopReceipt> receipt;
    std::optional<CpuSnapshot> stopped_snapshot;
    StopReason last_reason{StopReason::Returned};
};

struct ThreadInterruptBinding final {
    FEXCore::Context::Context *fex{};
    FEXCore::Core::InternalThreadState *native{};
    std::uintptr_t fault_page{};
    std::uintptr_t stop_spill{};
    // NON-spill dispatcher stop entry used by the syscall-fault immediate exit (restores the
    // dispatcher's host callee-saved save area without re-spilling). Read on the owner thread.
    std::uintptr_t stop_no_spill{};
    // Set by HandleSyscall on this owner thread when the syscall has no valid HLE path; the syscall
    // wrapper reads it to take the immediate-exit branch instead of running the in-block successor.
    std::atomic<bool> syscall_fault_pending{false};
    bool hle_pending{};
    // Structured syscall-fault attribution for this crossing (N4). Filled by HandleSyscall on the
    // owner thread when fault_pending is set; consumed by BuildRunResultLocked after Run returns.
    // Identity (context/thread gen/invocation) is seeded by Run before ExecuteThread; the fault
    // fields are written by the handler. Lives on the owner binding so it cannot be read by another
    // owner and is dropped with the binding at Run teardown.
    struct SyscallFaultEvent {
        bool present{false};
        std::uint64_t operation{};
        std::uint64_t fault_guest_rip{};
        std::uint64_t context_id{};
        std::uint64_t thread_id{};
        std::uint64_t thread_generation{};
        std::uint64_t invocation_id{};
        ErrorCategory category{ErrorCategory::None};
        int system_error{0};
        bool has_error{false};
    } syscall_fault_event{};
    std::array<std::uint64_t, 31> gprs{};
    std::uint64_t pstate{};
    std::uint64_t guest_rip{};
    std::atomic<bool> interrupted{false};
    // The owner's real host FP environment, saved by Run before switching to the guest FPCR/FPSR.
    // HandleSyscall installs it around the native call so the native function sees host rounding;
    // the guest environment is restored before the guest resumes (per-crossing host/guest FP).
    fenv_t owner_host_fenv{};
    bool owner_host_fenv_valid{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);
thread_local ThreadInterruptBinding *t_binding = nullptr;
struct sigaction g_previous_fault_action {};
struct sigaction g_previous_bus_action {};
std::mutex g_interrupt_install_lock;
bool g_interrupt_installed{};

void ForwardAction(int signal, siginfo_t *info, void *ucontext, const struct sigaction &previous) {
    if (previous.sa_handler == SIG_IGN)
        return;
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == nullptr) {
        ::signal(signal, SIG_DFL);
        ::raise(signal);
    } else if ((previous.sa_flags & SA_SIGINFO) != 0) {
        previous.sa_sigaction(signal, info, ucontext);
    } else {
        previous.sa_handler(signal);
    }
}

void InterruptFaultHandler(int signal, siginfo_t *info, void *raw_context) {
#if defined(__aarch64__)

    auto *binding = t_binding;
    if (binding && info && info->si_code == SEGV_ACCERR &&
        reinterpret_cast<std::uintptr_t>(info->si_addr) == binding->fault_page) {

        auto *uc = static_cast<ucontext_t *>(raw_context);
        const auto pc = uc->uc_mcontext.pc;
        const bool in_jit = binding->fex->IsAddressInCodeBuffer(binding->native, pc);
        if (!in_jit) {

            // FEX's deferred-signal guards also store zero to this exclusively
            // owned page after host work. Skip that probe, NOT its host frame;
            // keep the page protected until the next JIT entry. AArch64 stores
            // are one instruction. This address is never exposed to guest/HLE.
            uc->uc_mcontext.pc += 4;
            return;
        }
        // An IR-internal backward edge (e.g. REP) may also contain this probe,
        // even with MULTIBLOCK disabled. Only the FIRST probe after this block's
        // header is an entry safe point. Skip internal probes without unwinding
        // partially executed guest instructions. This encoding/layout is pinned
        // to the same FEX revision as the adapter, not a generic PC heuristic.
        constexpr auto offset = offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) -
                                offsetof(FEXCore::Core::InternalThreadState, BaseFrameState);
        static_assert(offset <= 32760 && offset % 8 == 0);
        constexpr std::uint32_t probe = 0xf9000000u | (offset / 8 << 10) | (28 << 5) | 31;
        const auto header = binding->native->CurrentFrame->State.InlineJITBlockHeader;
        bool entry_probe = false;
        // The entry preamble is short. Refuse rather than guess if a future FEX
        // layout moves it beyond this bounded range.
        for (auto at = header + sizeof(FEXCore::CPU::CPUBackend::JITCodeHeader);
             at <= pc && at < header + 256; at += 4) {
            if (*reinterpret_cast<const std::uint32_t *>(at) == probe) {
                entry_probe = at == pc;
                break;
            }
        }
        if (!entry_probe) {

            uc->uc_mcontext.pc += 4;
            return;
        }

        // MULTIBLOCK is disabled and the fault check is before guest operations.
        // InlineJITBlockHeader was installed by EmitEntryPoint immediately before
        // this store. Preserve NZCV/GPRs for FEX's flag reconstruction on return.
        binding->guest_rip = binding->fex->GetGuestBlockEntry(binding->native);
        binding->pstate = uc->uc_mcontext.pstate;
        for (std::size_t i = 0; i < binding->gprs.size(); ++i)
            binding->gprs[i] = uc->uc_mcontext.regs[i];
        binding->interrupted.store(true, std::memory_order_release);
        uc->uc_mcontext.sp = binding->native->CurrentFrame->ReturningStackLocation;
        uc->uc_mcontext.regs[28] = reinterpret_cast<std::uintptr_t>(binding->native->CurrentFrame);
        uc->uc_mcontext.pc = binding->stop_spill;
        return;
    }
    if (info && info->si_code == SEGV_ACCERR) {
        const int saved_errno = errno;
        const bool handled = AccessFaultHandler::Dispatch(raw_context, info->si_addr);
        errno = saved_errno;
        if (handled) return;
    }
    // A JIT PC alone does not prove a guest access fault or an exact guest RIP.
    // Emit a bounded, allocation-free diagnostic and forward unchanged. Do not
    // call Android logging (locks/allocation) or spill mid-instruction state.
    if (binding && info && (info->si_code == SEGV_MAPERR || info->si_code == SEGV_ACCERR)) {
        auto* uc = static_cast<ucontext_t*>(raw_context);
        const auto pc = uc->uc_mcontext.pc;
        if (binding->fex->IsAddressInCodeBuffer(binding->native, pc)) {
            const int saved_errno = errno;
            char message[192];
            size_t length{};
            auto append = [&](const char* text) {
                while (*text && length < sizeof(message))
                    message[length++] = *text++;
            };
            auto hex = [&](std::uint64_t value) {
                append("0x");
                for (int shift = 60; shift >= 0; shift -= 4)
                    message[length++] = "0123456789abcdef"[(value >> shift) & 15];
            };
            append("FEX JIT fault (block attribution only): block=");
            hex(binding->fex->GetGuestBlockEntry(binding->native));
            append(" address=");
            hex(reinterpret_cast<uintptr_t>(info->si_addr));
            append(" host_pc=");
            hex(pc);
            append("\n");
            (void)::write(STDERR_FILENO, message, length);
            errno = saved_errno;
        }
    }
#endif
    ForwardAction(signal, info, raw_context, g_previous_fault_action);
}

// x86 permits unaligned atomic and vector accesses that the FEX JIT translates to
// AArch64 instructions which fault with SIGBUS/BUS_ADRALN (exclusive monitors and
// some vector ops require alignment). FEX ships a backpatch handler that rewrites
// the faulting instruction to an unaligned-safe sequence and reports how far to
// advance the PC to retry. Without it a game that does an unaligned atomic (real
// TMNT libc/module init does) takes a hard host SIGBUS instead of continuing.
// This runs on the owner thread inside the JIT, so t_binding is set; only a fault
// whose PC is inside this thread's code buffer is backpatched, everything else is
// forwarded unchanged.
void UnalignedFaultHandler(int signal, siginfo_t *info, void *raw_context) {
#if defined(__aarch64__)
    auto *binding = t_binding;
    if (binding && info && info->si_code == BUS_ADRALN) {
        auto *uc = static_cast<ucontext_t *>(raw_context);
        const auto pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
        if (binding->fex->IsAddressInCodeBuffer(binding->native, pc)) {
            auto *regs = reinterpret_cast<std::uint64_t *>(uc->uc_mcontext.regs);
            const auto adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
                binding->native, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc,
                regs);
            if (adjustment.has_value()) {
                uc->uc_mcontext.pc = pc + *adjustment;
                return;
            }
        }
    }
#endif
    ForwardAction(signal, info, raw_context, g_previous_bus_action);
}

Status InstallInterruptHandler() {
    std::lock_guard guard{g_interrupt_install_lock};
    if (g_interrupt_installed)
        return Ok();
    struct sigaction action {};
    action.sa_sigaction = InterruptFaultHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGSEGV, &action, &g_previous_fault_action) != 0)
        return BackendError(ErrorCategory::BackendFailure, "InstallInterruptHandler",
                            "sigaction(SIGSEGV) failed", errno);
    // The JIT's unaligned atomic/vector accesses arrive as SIGBUS; back them up
    // with FEX's unaligned handler on the same altstack.
    struct sigaction bus_action {};
    bus_action.sa_sigaction = UnalignedFaultHandler;
    bus_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&bus_action.sa_mask);
    if (::sigaction(SIGBUS, &bus_action, &g_previous_bus_action) != 0) {
        const int saved = errno;
        ::sigaction(SIGSEGV, &g_previous_fault_action, nullptr);
        return BackendError(ErrorCategory::BackendFailure, "InstallInterruptHandler",
                            "sigaction(SIGBUS) failed", saved);
    }
    g_interrupt_installed = true;
    return Ok();
}

void RestoreInterruptHandler() {
    std::lock_guard guard{g_interrupt_install_lock};
    if (!g_interrupt_installed)
        return;
    ::sigaction(SIGSEGV, &g_previous_fault_action, nullptr);
    ::sigaction(SIGBUS, &g_previous_bus_action, nullptr);
    g_interrupt_installed = false;
}

// An owner restores its pre-existing altstack on every Run exit. Allocated before
// execution, never in the handler. An existing sufficiently sized stack is reused.
class OwnerSignalStack final {
  public:
    Status Install(std::size_t page_size) {
        if (::sigaltstack(nullptr, &previous_) != 0)
            return BackendError(ErrorCategory::BackendFailure, "Run", "query altstack", errno);
        if ((previous_.ss_flags & SS_ONSTACK) != 0)
            return BackendError(ErrorCategory::WrongState, "Run", "cannot Run from a signal stack");
        if (!(previous_.ss_flags & SS_DISABLE) && previous_.ss_size >= 64 * 1024)
            return Ok();
        size_ = 128 * 1024 + 2 * page_size;
        memory_ = ::mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory_ == MAP_FAILED) {
            memory_ = nullptr;
            return BackendError(ErrorCategory::OutOfMemory, "Run", "allocate altstack", errno);
        }
        auto *stack = static_cast<std::byte *>(memory_) + page_size;
        if (::mprotect(stack, 128 * 1024, PROT_READ | PROT_WRITE) != 0)
            return BackendError(ErrorCategory::BackendFailure, "Run", "protect altstack", errno);
        stack_t replacement{};
        replacement.ss_sp = stack;
        replacement.ss_size = 128 * 1024;
        if (::sigaltstack(&replacement, nullptr) != 0)
            return BackendError(ErrorCategory::BackendFailure, "Run", "install altstack", errno);
        installed_ = true;
        return Ok();
    }
    ~OwnerSignalStack() {
        if (installed_)
            ::sigaltstack(&previous_, nullptr);
        if (memory_)
            ::munmap(memory_, size_);
    }

  private:
    stack_t previous_{};
    void *memory_{};
    std::size_t size_{};
    bool installed_{};
};

// LookupCache's constructor calls SyscallHandler::MarkOvercommitRange, so
// CreateThread crashes without a handler. Three methods are pure virtual and
// must be supplied even though guest syscalls are not part of V0.
// --- syscall-fault immediate exit (production) --------------------------------
// The fixed non-Windows FEX syscall flags have no BLOCK_END, so `syscall; successor; return-gate`
// compiles into one JIT block: a normal return from HandleSyscall falls through to the successor.
// The JIT calls SyscallHandlerFunc with the standard AArch64 C ABI: x0 = SyscallHandlerObj,
// x1 = CpuStateFrame*, via an indirect blr (BranchOps.cpp ~301). Run installs THIS function in
// CurrentFrame->Pointers.SyscallHandlerFunc for every guest thread; it forwards to the original
// handler, and when the syscall has no valid HLE path it leaves ExecuteThread at the syscall
// instruction (successor never runs) by switching, on the owner thread after the C++ dispatch fully
// returned, to the dispatcher's NON-spill stop entry. Per-thread state is the owner's t_binding
// (the wrapper runs on the owner thread); no process-wide slot drives the fault decision.
struct FexSyscallOriginal {
    void* obj{};
    void (*func)(void*, FEXCore::Core::CpuStateFrame*){};
};
thread_local FexSyscallOriginal t_syscall_original{};
#if defined(GUEST_CPU_TEST_HOOKS)
// Optional syscall-point trace for the N3 probe. The record type is declared in fex_context.h.
thread_local bool t_syscall_trace_enabled{};
FexSyscallTraceRecord g_syscall_trace_probe{};
#endif

extern "C" void FexSyscallWrapper(void* obj, FEXCore::Core::CpuStateFrame* frame);

// Naked AArch64 exit boundary, entered only after the C++ dispatch has returned and its automatic
// objects are destroyed (no C++ cleanup skipped). x28 <- frame, sp <- ReturningStackLocation, branch
// to the non-spill stop entry which pops the dispatcher callee-saved save area and returns from
// ExecuteThread into Run's C++ epilogue. Does not return.
[[noreturn]] __attribute__((naked)) void FexSyscallExitToStop(void* frame, void* stop, void* sp) {
#if defined(__aarch64__)
    asm volatile("mov x28, x0\n\t"
                 "mov sp, x2\n\t"
                 "br x1\n\t"
                 ::: "x0", "x1", "x2", "x28", "memory", "cc");
#endif
}

// C++ decision point: forward to the JIT-installed handler, then check the owner binding's fault
// flag. Returns 0 = continue into the JIT epilogue (successor runs); 1 = fault exit.
static int FexSyscallDispatch(void* obj, FEXCore::Core::CpuStateFrame* frame) {
    if (auto* func = t_syscall_original.func)
        func(obj, frame);
#if defined(GUEST_CPU_TEST_HOOKS)
    if (t_syscall_trace_enabled) {
        std::uint64_t host_sp = 0, host_x28 = 0, host_lr = 0;
        asm volatile("mov %0, sp" : "=r"(host_sp));
        asm volatile("mov %0, x28" : "=r"(host_x28));
        asm volatile("mov %0, x30" : "=r"(host_lr));
        auto& tr = g_syscall_trace_probe;
        tr.shim_sp = host_sp; tr.shim_x28 = host_x28; tr.shim_lr = host_lr;
        tr.returning_stack = frame->ReturningStackLocation;
        tr.in_syscall = frame->InSyscallInfo;
        tr.guest_rip = frame->State.rip;
        tr.guest_rcx = frame->State.gregs[FEXCore::X86State::REG_RCX];
        tr.guest_r11 = frame->State.gregs[FEXCore::X86State::REG_R11];
        tr.callret_sp = frame->State.callret_sp;
        tr.frame_addr = reinterpret_cast<std::uint64_t>(frame);
        ++tr.invocations;
    }
#endif
    auto* binding = t_binding;
    if (binding &&
        (binding->hle_pending || binding->syscall_fault_pending.load(std::memory_order_acquire))) {
        // The JIT syscall epilogue that clears InSyscallInfo is bypassed; clear it here. Guest RIP
        // already holds the syscall PC (SyscallOp stored NewRIP before _Syscall; the in-block
        // successor does not reload it).
        frame->InSyscallInfo = 0;
        return 1;
    }
    return 0;
}

// Installed as SyscallHandlerFunc for every guest Run. A normal C++ function is enough: the JIT blr
// is a standard C ABI call, so the compiler owns this frame and all C++ cleanup completes before the
// fault branch switches stacks.
extern "C" void FexSyscallWrapper(void* obj, FEXCore::Core::CpuStateFrame* frame) {
    if (FexSyscallDispatch(obj, frame) == 1) {
        const std::uintptr_t stop = t_binding ? t_binding->stop_no_spill : 0;
        FexSyscallExitToStop(frame, reinterpret_cast<void*>(stop),
                             reinterpret_cast<void*>(frame->ReturningStackLocation));
    }
}

class FexSyscallHandler final : public FEXCore::HLE::SyscallHandler {
  public:
    explicit FexSyscallHandler(GuestAddressSpace& space) : space_(space) {}

    // Real guest->host HLE gate (R2-H01). The guest places the operation number in rax before the
    // syscall instruction; its other GPRs/xmm hold the SysV arguments. We switch FP environment,
    // build a frame from the spilled state and dispatch to a registered native function; the return
    // value is encoded back into the frame and the guest continues after the syscall. An
    // unregistered operation is a per-thread fault, never an implicit host syscall.
    void HandleSyscall(FEXCore::Core::CpuStateFrame* frame) override {
        if (frame && t_binding && registry_.Find(frame->State.gregs[FEXCore::X86State::REG_RAX])) {
            // Exit ExecuteThread before running C++ HLE. No outer JIT/C++ frame
            // remains suspended when a host HLE waits, changes VM or calls guest.
            t_binding->hle_pending = true;
            return;
        }
        DispatchNative(frame, t_binding, false);
    }

    void DispatchNative(FEXCore::Core::CpuStateFrame* Frame, ThreadInterruptBinding* binding,
                        bool outside_jit) {
        if (Frame == nullptr) {
            unknown_thread_syscall_.store(true, std::memory_order_release);
            return;
        }

        // Per-thread attribution first: a fault flag unique to this thread.
        std::shared_ptr<std::atomic<bool>> fault_flag;
        {
            std::lock_guard guard{threads_lock_};
            auto it = syscall_fault_by_frame_.find(Frame);
            if (it != syscall_fault_by_frame_.end()) {
                fault_flag = it->second.lock();
            }
        }

        const std::uint64_t operation = Frame->State.gregs[FEXCore::X86State::REG_RAX];
        // Preserve the architectural RCX across the crossing. The 4th syscall integer arrives in R10
        // and RCX is only a *decode view* for the typed adapter; RCX is not a syscall return field,
        // so it must keep the guest's post-syscall successor value. Without this, encoding the R10
        // view into the full register file and writing it back clobbered RCX (measured: RCX became
        // the R10 marker after a valid call).
        const std::uint64_t guest_rcx = Frame->State.gregs[FEXCore::X86State::REG_RCX];
        bool rejected = false;
        Error reject_err{};
        if (auto adapter = registry_.Find(operation)) {
            Hle::HleCallFrame hle_frame{};
            hle_frame.operation = operation;
            hle_frame.space = &space_;
            RegistersFromCpuState(Frame->State, hle_frame.registers);
            // syscall callgate: the 4th integer argument arrived in r10; the adapter decodes rcx
            // positionally, so normalise r10 -> rcx in the decode view only (not written back below).
            hle_frame.registers.Set(Gpr::Rcx, Frame->State.gregs[FEXCore::X86State::REG_R10]);
            hle_frame.rcx_normalised_from_r10 = true;

            // Per-crossing host/guest FP boundary. Install the OWNER's real host environment (saved
            // by Run before it loaded the guest FPCR), not whatever fegetenv would read here (which
            // is the guest environment); then invoke the native function. Restore the guest FP state
            // (derived from its MXCSR, the same mapping Run uses) before the guest resumes.
            const auto install_guest_fenv = [&frame = Frame->State, outside_jit] {
                if (outside_jit)
                    return;
                const std::uint64_t mx = frame.mxcsr;
                const std::uint64_t r = (mx >> 13) & 3;
                const std::uint64_t fpcr =
                    (((r & 1) << 1) | ((r & 2) >> 1)) << 22 |
                    (static_cast<std::uint64_t>((mx >> 15) & 1) << 24);
                asm volatile("msr fpcr, %0\n\tmsr fpsr, xzr" : : "r"(fpcr) : "memory");
            };

            fenv_t owner_fp{};
            const bool have_owner_fp = binding && binding->owner_host_fenv_valid;
            if (have_owner_fp)
                owner_fp = binding->owner_host_fenv;
            else
                ::fegetenv(&owner_fp);
            ::fesetenv(&owner_fp);

            // Exception boundary: a native HLE exception must NOT unwind across the JIT (that aborts
            // the process, observed as libc++abi terminate / exit 134). Catch it after the native
            // stack unwound to this C++ frame, finish this frame's cleanup, and route it through the
            // same syscall-fault immediate exit as a rejected call, attributed to this owner.
            Status call_status;
            bool native_threw = false;
            try {
                call_status = adapter->Invoke(hle_frame);
            } catch (const std::exception& ex) {
                native_threw = true;
                call_status = BackendError(ErrorCategory::BackendFailure, "HLE",
                                           std::string{"native HLE threw: "} + ex.what());
            } catch (...) {
                native_threw = true;
                call_status = BackendError(ErrorCategory::BackendFailure, "HLE",
                                           "native HLE threw an unknown exception");
            }
            // Back to the guest rounding for any guest instruction that runs after the call.
            install_guest_fenv();

            if (call_status && !native_threw) {
                // The decode-only RCX view must not leak into the architectural state: restore the
                // guest RCX before encoding return values, then write back only the registers the
                // adapter actually produced.
                hle_frame.registers.Set(Gpr::Rcx, guest_rcx);
                ApplyRegistersToCpuState(hle_frame.registers, Frame->State);
                return;
            }
            if (native_threw || !call_status) {
                // A native exception or a registered-but-rejected call (bad pointer/signature) is
                // this thread's fault and takes the immediate-exit path.
                rejected = true;
                reject_err = call_status.GetError();
                last_hle_error_.store(reject_err.category, std::memory_order_release);
            }
        }

        if (fault_flag) {
            fault_flag->store(true, std::memory_order_release);
        } else {
            unknown_thread_syscall_.store(true, std::memory_order_release);
        }
        // Signal the production syscall wrapper (same owner thread, owner binding) that this syscall
        // faulted, so it takes the immediate-exit branch instead of running the in-block successor,
        // and record the structured attribution event for BuildRunResultLocked.
        if (binding) {
            binding->syscall_fault_pending.store(true, std::memory_order_release);
            auto& ev = binding->syscall_fault_event;
            ev.present = true;
            ev.operation = operation;
            ev.fault_guest_rip = Frame->State.rip;
            ev.category = rejected ? reject_err.category : ErrorCategory::InvalidArgument;
            ev.system_error = static_cast<int>(reject_err.system_error);
            ev.has_error = rejected;
        }
    }

    Hle::HleCallRegistry& Registry() { return registry_; }

    // Registers the per-thread syscall-fault flag for the duration of an ExecuteThread call. The
    // weak_ptr entry is dropped when the owner unregisters, so a stale frame never faults the
    // wrong thread and a destroyed thread's flag does not outlive it.
    void RegisterThreadFrame(FEXCore::Core::CpuStateFrame* frame,
                             std::weak_ptr<std::atomic<bool>> flag) {
        std::lock_guard guard{threads_lock_};
        syscall_fault_by_frame_[frame] = std::move(flag);
    }
    void UnregisterThreadFrame(FEXCore::Core::CpuStateFrame* frame) {
        std::lock_guard guard{threads_lock_};
        syscall_fault_by_frame_.erase(frame);
    }

    [[nodiscard]] bool TakeUnknownThreadSyscall() {
        return unknown_thread_syscall_.exchange(false, std::memory_order_acq_rel);
    }

    FEXCore::HLE::ExecutableRangeInfo
    QueryGuestExecutableRange(FEXCore::Core::InternalThreadState *Thread,
                              uint64_t Address) override {
        std::lock_guard guard{lock};
        for (const auto &range : executable_ranges) {
            if (Address >= range.base && Address < range.base + range.size) {
                return {.Base = range.base, .Size = range.size, .Writable = range.writable};
            }
        }
        auto mapping = space_.Query(GuestAddress{Address});
        if (mapping && HasPermission(mapping.Value().permission, GuestPermission::Execute))
            return {.Base = mapping.Value().range.base.value, .Size = mapping.Value().range.size,
                    .Writable = HasPermission(mapping.Value().permission, GuestPermission::Write)};
        // Not a known executable range. Report an empty one rather than
        // claiming the address is valid code.
        return {.Base = Address, .Size = 0, .Writable = false};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo>
    LookupExecutableFileSection(FEXCore::Core::InternalThreadState *Thread,
                                uint64_t GuestAddr) override {
        // No file-backed guest mappings in V0; the disk code cache stays off.
        return std::nullopt;
    }

    void PreCompile() override {
        // CompileBlock always calls this, so it is a reliable signal that the dispatcher actually
        // reached translation rather than exiting first.
        compile_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Where an interrupted owner actually waits.
    //
    // FEXCore's dispatcher calls this after the pause stub has spilled the static register
    // allocation, so by the time we are here the thread's CPUState is complete and a snapshot taken
    // from it is authoritative. Returning from this function makes the stub execute its hlt, which
    // faults back in to restore and resume -- see docs/fex-async-stop-source-proof.md.
    //
    // The default implementation in FEXCore is an empty body, so before this override a pause
    // signal would spill and immediately resume: the thread would never actually stop.
    // FEXCore calls this once per guest page it has compiled code from, which is the only
    // outside-visible confirmation of *what* was translated.
    void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState *Thread, uint64_t Start,
                                  uint64_t Length) override {
        if (::getenv("GUEST_CPU_DEBUG") != nullptr) {
            std::fprintf(stderr, "[guest_cpu] compiled code covering 0x%llx +0x%llx\n",
                         static_cast<unsigned long long>(Start),
                         static_cast<unsigned long long>(Length));
        }
    }

    [[nodiscard]] std::uint64_t CompileCount() const {
        return compile_count.load(std::memory_order_relaxed);
    }

    void RegisterExecutableRange(std::uint64_t base, std::uint64_t size, bool writable) {
        std::lock_guard guard{lock};
        executable_ranges.push_back({base, size, writable});
    }

    [[nodiscard]] bool TakeUnknownThreadSyscallGlobal() {
        return TakeUnknownThreadSyscall();
    }

  private:
    struct Range final {
        std::uint64_t base{};
        std::uint64_t size{};
        bool writable{};
    };

    mutable std::mutex lock;
    std::vector<Range> executable_ranges; // Immutable backend return gate only.
    GuestAddressSpace& space_;

    // Per-thread syscall-fault attribution (R2-H05). Keyed by the frame FEXCore passes to
    // HandleSyscall; weak so a thread that has exited never keeps the map entry alive.
    mutable std::mutex threads_lock_;
    std::unordered_map<FEXCore::Core::CpuStateFrame*, std::weak_ptr<std::atomic<bool>>>
        syscall_fault_by_frame_;
    std::atomic<bool> unknown_thread_syscall_{false};
    std::atomic<ErrorCategory> last_hle_error_{ErrorCategory::None};

    // Registered native HLE functions. An embedder installs typed adapters here; a guest syscall
    // whose rax names a registered operation is dispatched instead of faulting.
    Hle::HleCallRegistry registry_;
    std::atomic<std::uint64_t> compile_count{0};
};

// Upper bound this backend places on guest addresses.
//
// This is a V0 policy, NOT a demonstrated FEXCore capability limit. An earlier revision claimed
// The pinned lookup compares full guest tags after masking the cache index.
// G46 executes and republishes blocks separated by 64 GiB in one owner. Keep a
// bounded 128 GiB embedder policy for the production PS4 layout; this is not a
// claim that every x86-64 canonical address or 16 KiB host configuration is tested.
constexpr std::uint64_t kGuestAddressPolicyLimit = std::uint64_t{1} << 37;

// --- return gate -------------------------------------------------------------
// A host page holding a single x86 HLT, mapped executable and registered as a
// guest executable range. Guest code returns to this address; with
// EnableExitOnHLT the HLT makes ExecuteThread return instead of trapping.
//
// This is what makes StopReason::Returned distinguishable from a real guest HLT
// (acceptance D05): only this exact address counts as a normal return, and a
// HLT anywhere else is a fault.
class ReturnGate final {
  public:
    ~ReturnGate() {
        if (page != nullptr && page != MAP_FAILED) {
            ::munmap(page, size);
        }
    }

    [[nodiscard]] bool Create(std::string &error_detail, int &error_no) {
        const long host_page = ::sysconf(_SC_PAGESIZE);
        size = host_page > 0 ? static_cast<std::size_t>(host_page) : 4096;

        // The gate is guest code and must stay within the verified embedder VA policy.
        //
        // Hinted near the top of the addressable range, because the guest reservation is placed in
        // the lower half and a hint that lands inside it would be rejected and fall back to a high
        // address. A hint is advisory either way, so the result is verified below.
        void *hint = reinterpret_cast<void *>(kGuestAddressPolicyLimit - (std::uint64_t{1} << 30));
        page = ::mmap(hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            error_no = errno;
            error_detail = "failed to map the return gate page";
            page = nullptr;
            return false;
        }
        if (reinterpret_cast<std::uint64_t>(page) + size > kGuestAddressPolicyLimit) {
            ::munmap(page, size);
            page = nullptr;
            error_detail = "the kernel placed the return gate above the addressable guest range";
            return false;
        }

        // 0xF4 == HLT.
        *static_cast<std::uint8_t *>(page) = 0xF4;

        // W^X: publish read+execute only after the byte is written. Writing
        // through an RWX mapping would also work on Linux but is refused under
        // stricter policies and is not needed here.
        if (::mprotect(page, size, PROT_READ | PROT_EXEC) != 0) {
            error_no = errno;
            error_detail = "failed to make the return gate executable";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t Address() const noexcept {
        return reinterpret_cast<std::uint64_t>(page);
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size; }

  private:
    void *page{};
    std::size_t size{};
};

// --- call-return stack -------------------------------------------------------
// FEXCore reads InternalThreadState::CallRetStackBase and CPUState::callret_sp but never allocates
// the memory behind them: that is the embedder's job, like the signal delegator and syscall
// handler. Leaving it null makes the JIT fault on its first call/ret, which surfaces as an
// immediate exit rather than as an obvious null dereference.
//
// Reserved PROT_NONE with a guard page on each side, then only the middle made writable, so an
// overrun or underrun of the call-return stack faults instead of corrupting a neighbour.
class CallRetStack final {
  public:
    ~CallRetStack() {
        if (reservation != nullptr && reservation != MAP_FAILED) {
            ::munmap(reservation, TotalSize());
        }
    }

    [[nodiscard]] bool Create(std::string &error_detail, int &error_no) {
        const long host_page = ::sysconf(_SC_PAGESIZE);
        guard_size = host_page > 0 ? static_cast<std::size_t>(host_page) : 4096;

        reservation = ::mmap(nullptr, TotalSize(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) {
            error_no = errno;
            error_detail = "failed to reserve the call-return stack";
            reservation = nullptr;
            return false;
        }
        if (::mprotect(Base(), FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE,
                       PROT_READ | PROT_WRITE) != 0) {
            error_no = errno;
            error_detail = "failed to make the call-return stack writable";
            return false;
        }
        return true;
    }

    void InstallInto(FEXCore::Core::InternalThreadState *thread) const {
        thread->CallRetStackBase = Base();
        // Start a quarter in, matching the reference bring-up: the stack grows in both directions
        // depending on call depth, so starting at either end would waste half of it.
        thread->CurrentFrame->State.callret_sp =
            reinterpret_cast<std::uint64_t>(Base()) +
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
    }

  private:
    [[nodiscard]] std::size_t TotalSize() const {
        return FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * guard_size;
    }
    [[nodiscard]] void *Base() const {
        return static_cast<std::uint8_t *>(reservation) + guard_size;
    }

    void *reservation{};
    std::size_t guard_size{};
};

// --- context -----------------------------------------------------------------
#if defined(GUEST_CPU_TEST_HOOKS)
// Deterministic Run-entry delay for coordinator tests. Test-only; compiled out of release builds.
//
// The owner calls WaitAtEntry while it holds no coordinator/context/address-space lock and before
// entering the JIT. It blocks while an arm for its thread id is live; the controller arms it, waits
// for arrival, exercises the coordinator (which sends the Pause to a running thread that cannot yet
// stop), observes the second owner stop independently in real JIT, then Release(). A strict
// arrival/arm/exit generation handshake means the controller can never arm a hold before the prior
// one has fully left, and the owner never parks inside a signal handler or with a FEX lock held.
// Thin adapter over the shared, standard-library-only protocol implementation. Keeping the protocol
// in tests/guest_cpu/test_run_gate.h lets a host determinism unit test exercise exactly this state
// machine without linking FEX; the on-device suite and host test run one and the same code.
class FexTestRunGateImpl final : public FexTestRunGate {
  public:
    std::uint64_t Arm(std::uint64_t thread_id, std::uint64_t context_id) override {
        return gate_.Arm(thread_id, context_id);
    }
    bool Arrived(std::uint64_t token) override { return gate_.Arrived(token); }
    bool Release(std::uint64_t token, std::uint64_t timeout_ms) override {
        return gate_.Release(token, timeout_ms);
    }
    bool Exited(std::uint64_t token) override { return gate_.Exited(token); }
    std::uint64_t BoundInvocation(std::uint64_t token) override { return gate_.BoundInvocation(token); }
    bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id,
                     std::uint64_t invocation, std::uint64_t timeout_ms) override {
        return gate_.WaitAtEntry(context_id, thread_id, invocation, timeout_ms);
    }
  private:
    Core::GuestCpu::TestGate::RunGate gate_;
};
#endif

class FexCpuContext final : public CpuContext, public CodeInvalidationSink {
  public:
    explicit FexCpuContext(const CpuConfig &config, GuestAddressSpace &space)
        : config_(config), space_(space) {}

    ~FexCpuContext() override {
        // Unregister before anything else: once the backend is going away, the address space must
        // stop routing publications to it rather than calling into a destroyed object.
        space_.ClearCodeInvalidationSink(this);
        // Destroy threads before the context: FEXCore requires it, and a live
        // thread holding a code buffer would otherwise outlive its owner.
        {
            std::lock_guard guard{lock_};
            for (auto &[id, entry] : threads_) {
                if (entry.native != nullptr && context_ != nullptr) {
                    context_->DestroyThread(entry.native);
                    entry.native = nullptr;
                }
            }
            threads_.clear();
        }
        context_.reset();
        RestoreInterruptHandler();
        g_context_active.store(false, std::memory_order_release);
    }

    [[nodiscard]] Result<void> Initialize() {
        std::string detail;
        int error_no = 0;
        if (!return_gate_.Create(detail, error_no)) {
            return BackendError(ErrorCategory::OutOfMemory, "CreateContext", detail, error_no);
        }

        // FEXCore has no sysconf call in its link scope, so the host page size
        // must be injected or it silently assumes 4096.
        const long host_page = ::sysconf(_SC_PAGESIZE);
        if (host_page <= 0) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "sysconf(_SC_PAGESIZE) did not report a usable page size");
        }
        host_page_size_ = static_cast<std::uint64_t>(host_page);
        if (!FEXCore::Utils::SetHostPageSize(host_page_size_)) {
            return BackendError(ErrorCategory::Unsupported, "CreateContext",
                                "this FEXCore build does not support the host page size");
        }

        FEXCore::Config::Initialize();
        // Order matters: ReloadMetaLayer rebuilds the meta layer from the registered config layers
        // and drops anything Set beforehand. Setting after it is what makes the value stick.
        //
        // Getting this backwards left Is64BitMode unset, which ContextImpl reads as 32-bit: it then
        // clamps VirtualMemSize to 1<<32 and the decoder builds 32-bit blocks from 64-bit guest
        // bytes. That was the cause of guest fixtures having no architectural effect.
        FEXCore::Config::ReloadMetaLayer();
        FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
        // Core-only GDBSERVER enables the entry interrupt-page store, not a server.
        // Single basic blocks make its fault a restartable architectural boundary.
        FEXCore::Config::Set(FEXCore::Config::CONFIG_GDBSERVER, "1");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "0");

        {
            // Read it back rather than assuming the write landed; a silently 32-bit context
            // mistranslates every guest instruction while still appearing to run.
            auto mode = FEXCore::Config::Get(FEXCore::Config::CONFIG_IS64BIT_MODE);
            if (!mode || **mode != "1") {
                return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                    "FEXCore did not accept 64-bit guest mode");
            }
        }

        // Optional JIT disassembly. Must be set here, before the Context is constructed, because
        // FEX_CONFIG_OPT caches the value at construction; setting the FEX_DISASSEMBLE environment
        // variable has no effect on an embedder that does not load FEX's environment config layer.
        // Requires a FEXCore built with -DENABLE_VIXL_DISASSEMBLER=ON.
        if (const char *disasm = ::getenv("GUEST_CPU_DISASSEMBLE"); disasm != nullptr) {
            FEXCore::Config::Set(FEXCore::Config::CONFIG_DISASSEMBLE, disasm);
        }

        // Surface FEXCore's own diagnostics. Without a handler these are dropped, and a JIT-side
        // refusal looks identical to a guest that simply did nothing. On Android stderr goes
        // nowhere, so the handlers additionally emit to logcat (tag "FexCore") where a device run
        // can capture assert text that would otherwise be invisible before a SIGILL/abort.
        if (::getenv("GUEST_CPU_DEBUG") != nullptr) {
            LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char *message) {
                std::fprintf(stderr, "[fex %s] %s\n", LogMan::DebugLevelStr(level), message);
#if defined(__ANDROID__)
                __android_log_print(ANDROID_LOG_ERROR, "FexCore", "[%s] %s",
                                    LogMan::DebugLevelStr(level), message);
#endif
            });
            LogMan::Throw::InstallHandler([](const char *message) {
                std::fprintf(stderr, "[fex assert] %s\n", message);
#if defined(__ANDROID__)
                __android_log_print(ANDROID_LOG_FATAL, "FexCore", "[assert] %s", message);
#endif
            });
        }

        // A native embedder already owns the guest reservation. FEXLoader's
        // SetupHooks steals all high-VA gaps, starving bionic/ART, pthread stacks
        // and native services. Initialize the pinned rpmalloc implementation with
        // ordinary host mmap/munmap instead; kernel placement excludes our owned
        // guest reservation. Do not replace global libc allocation or mmap hooks.
        // InitializeAllocator is a pinned private API, just like CPUBackend above.
        static std::once_flag allocator_once;
        std::call_once(allocator_once, [&] {
            FEXCore::Allocator::SetupAllocatorHooks(::mmap, ::munmap);
            FEXCore::Allocator::InitializeAllocator(host_page_size_);
        });

        // Reads the real ID registers rather than assuming a feature set.
        host_features_ = FEX::FetchHostFeatures();
        context_ = FEXCore::Context::Context::CreateNewContext(host_features_);
        if (!context_) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore refused to create a context");
        }

        signal_delegator_ = std::make_unique<FexSignalDelegator>(return_gate_.Address());
        syscall_handler_ = std::make_unique<FexSyscallHandler>(space_);
        context_->SetSignalDelegator(signal_delegator_.get());
        context_->SetSyscallHandler(syscall_handler_.get());

        // Makes the return gate's HLT exit ExecuteThread rather than trap.
        //
        // GUEST_CPU_NO_EXIT_ON_HLT disables it for diagnosis: without it the gate's HLT takes the
        // SIGILL path, which deliberately faults, so a crash there is positive evidence that guest
        // execution actually reached the gate.
        if (::getenv("GUEST_CPU_NO_EXIT_ON_HLT") == nullptr) {
            context_->EnableExitOnHLT();
        }

        if (!context_->InitCore()) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore InitCore failed");
        }

        syscall_handler_->RegisterExecutableRange(return_gate_.Address(), return_gate_.Size(),
                                                  false);

        // The async kick handler. Installed after InitCore so the dispatcher config -- and with it
        // the spill entry points the handler needs -- already exists.
        if (auto installed = InstallInterruptHandler(); !installed) {
            return installed.GetError();
        }
        if (signal_delegator_->GetConfig().ThreadStopHandlerAddressSpillSRA == 0) {
            // Without this the handler has nowhere safe to redirect an in-JIT thread, and a stop
            // request would either do nothing or corrupt register state. Fail loudly at init
            // instead of at the first interrupt.
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore did not publish a pause spill entry point");
        }

        // Route the address space's publication transactions here. Done last, so a context that
        // failed to initialise is never registered as the thing that owns translated code.
        if (auto registered = space_.SetCodeInvalidationSink(this); !registered) {
            return registered;
        }
        return Result<void>{};
    }

    [[nodiscard]] BackendCapabilities Capabilities() const override {
        auto caps = QueryFexCapabilities();
        caps.host_page_size = host_page_size_;
        caps.memory_mode = config_.memory_mode;
        caps.smc_mode = config_.smc_mode;
        caps.return_gate_address = return_gate_.Address();
        return caps;
    }

    [[nodiscard]] Result<ThreadHandle> CreateThread(const ThreadInit &init) override {
        // Refuse while a publication transaction is active or code is poisoned. Creating a thread
        // is not executing yet, but it allocates backend state against a code image that is
        // mid-change, and admitting it here would let a Run follow immediately. Taking the lease
        // and dropping it at the end of this function is the admission check.
        std::lock_guard guard{lock_};
        auto admission = space_.AcquireExecutionLease();
        if (!admission) {
            return admission.GetError();
        }

        // The entry and stack must already be mapped in this context's address
        // space. Checking here turns a guest crash into a caller-side error.
        auto entry_mapping = space_.Query(GuestAddress{init.entry_rip.value});
        if (!entry_mapping) {
            return BackendError(ErrorCategory::InvalidArgument, "CreateThread",
                                "entry_rip is not mapped in this address space");
        }
        if (!HasPermission(entry_mapping.Value().permission, GuestPermission::Execute)) {
            return BackendError(ErrorCategory::PermissionDenied, "CreateThread",
                                "entry_rip is mapped without execute permission");
        }
        auto stack_mapping = space_.Query(init.initial_rsp);
        if (!stack_mapping) {
            return BackendError(ErrorCategory::InvalidArgument, "CreateThread",
                                "initial_rsp is not mapped in this address space");
        }

        FEXCore::Core::CPUState state{};
        state.rip = init.entry_rip.value;
        state.gregs[FEXCore::X86State::REG_RSP] = init.initial_rsp.value;
        ApplyPatchToState(init.initial_state, state);


        // Allocate the GDT before the thread so the pointer stored in CPUState
        // stays valid for the thread's whole life.
        auto gdt = std::make_unique<std::array<FEXCore::Core::CPUState::gdt_segment, 32>>();
        InitializeSegments(state, *gdt);

        auto *native = context_->CreateThread(&state);
        if (native)
            native->PassManager->InsertPass(fextl::make_unique<EntryBackedgePass>(),
                                            "ShadGuestEntryPoll");
        if (native == nullptr) {
            return BackendError(ErrorCategory::OutOfMemory, "CreateThread",
                                "FEXCore could not create a thread state");
        }

        // CreateThread copies the CPUState by value, so the live thread's copy
        // needs the segment pointers pointed at the same GDT again -- the copy
        // holds the address, but re-running this keeps the two in step if the
        // backend ever starts adjusting descriptors after creation.
        InitializeSegments(native->CurrentFrame->State, *gdt);

        // Must happen before the first Run: the JIT dereferences callret_sp on
        // its first call or ret.
        auto callret = std::make_unique<CallRetStack>();
        std::string detail;
        int error_no = 0;
        if (!callret->Create(detail, error_no)) {
            context_->DestroyThread(native);
            return BackendError(ErrorCategory::OutOfMemory, "CreateThread", std::move(detail),
                                error_no);
        }
        callret->InstallInto(native);

        if (init.initial_state.fields != RegisterValidity::None) {
            ApplyXmmPatch(init.initial_state, native);
        }

        const std::uint64_t id = next_thread_id_++;
        ThreadEntry entry{};
        entry.native = native;
        entry.generation = NextContextId();
        entry.owner = std::this_thread::get_id();
        entry.guest_tid = init.guest_tid;
        entry.entry_rip = init.entry_rip.value;
        entry.gdt = std::move(gdt);
        entry.callret = std::move(callret);

        const auto generation = entry.generation;
        auto [it, inserted] = threads_.emplace(id, std::move(entry));
        const ThreadHandle handle{id, generation};
        it->second.interrupt->stopped_snapshot =
            CaptureSnapshot(handle, it->second, SnapshotKind::SafePoint);
        return handle;
    }

    [[nodiscard]] Result<RunResult> Run(ThreadHandle thread, const RunOptions &options) override {
        return RunInternal(thread, options);
    }

    // The stopped-thread call subset shares Run's admission and locked entry
    // transition. It never recursively calls public Run or overwrites an active binding.
    [[nodiscard]] Result<RunResult> RunInternal(ThreadHandle thread, const RunOptions& options,
                                                const GuestCallArgs* call_args = nullptr,
                                                GuestCodeAddress call_entry = {},
                                                const GuestCallOptions& call_options = {}) {
        struct HostErrno {
            int value = errno;
            ~HostErrno() {
                errno = value;
            }
        } host_errno;
        auto* invoking_scope = Hle::HleScope::Current();
        const bool nested = call_args && invoking_scope && invoking_scope->CallbackAdmission() &&
                            &invoking_scope->Context() == this &&
                            invoking_scope->Thread() == thread;
        if (t_binding != nullptr || (invoking_scope && !nested))
            return BackendError(
                ErrorCategory::AlreadyRunning, "Run/InvokeGuest",
                "nested entry requires HleScope; this subset is stopped-thread only");
        bool owns_frame = false;
        std::uint64_t previous_invocation{};
        const auto retire_frame = [&](void*) {
            if (!owns_frame)
                return;
            std::lock_guard guard{lock_};
            if (auto* entry = FindOwnedLocked(thread)) {
                entry->running = false;
                entry->internally_parked = false;
                --entry->active_runs;
                if (entry->active_runs != 0)
                    entry->current_invocation = previous_invocation;
                if (entry->active_runs == 0) {
                    entry->running = false;
                    if (entry->interrupt->stopped_snapshot)
                        PublishReceiptLocked(thread, *entry);
                }
                stopped_changed_.notify_all();
            }
        };
        std::unique_ptr<void, decltype(retire_frame)> run_frame{reinterpret_cast<void*>(1),
                                                                retire_frame};
        std::optional<RegisterFile> saved_call_state;
        std::optional<PinnedSpan> call_stack;
        std::array<std::byte, 256> saved_stack_bytes{};
        if (options.deadline_ns != 0)
            return BackendError(ErrorCategory::Unsupported, "Run", "deadline_ns is unsupported");
        Result<ExecutionLease> lease{ExecutionLease{}};
        OwnerSignalStack signal_stack;
        if (auto status = signal_stack.Install(host_page_size_); !status)
            return status.GetError();

        ThreadInterruptBinding binding{};
        std::uint64_t invocation{};
        std::shared_ptr<std::atomic<bool>> syscall_fault;
        {
            std::unique_lock guard{lock_};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            for (;;) {
                auto admission = space_.AcquireExecutionLease();
                if (admission) {
                    lease.Value() = std::move(admission).Value();
                    break;
                }
                if (!config_.resume_internal_drains ||
                    admission.GetError().category != ErrorCategory::Busy)
                    return admission.GetError();
                if (std::chrono::steady_clock::now() >= deadline)
                    return BackendError(ErrorCategory::Timeout, "Run",
                                        "publication admission remained busy");
                guard.unlock();
                (void)space_.WaitForQuiescenceRelease(20'000'000);
                std::this_thread::yield();
                guard.lock();
            }
            // Admission and running-state publication share the coordinator lock.
            // BeginDrain cannot snapshot owners between these two operations.
            auto *entry = FindOwnedLocked(thread);
            if (!entry)
                return OwnershipError(thread, "Run");
            if (entry->running || (entry->active_runs != 0 && !nested) ||
                (nested && entry->active_runs == 0))
                return BackendError(ErrorCategory::AlreadyRunning, "Run",
                                    "no matching stopped HLE frame");
            if (entry->interrupt->last_reason == StopReason::GuestFault ||
                entry->interrupt->last_reason == StopReason::BackendFailure)
                return BackendError(ErrorCategory::WrongState, "Run",
                                    "destroy and recreate a faulted thread");
            if (options.resume_after_epoch != 0) {
                if (auto status = ConsumeResumeLocked(*entry, options.resume_after_epoch); !status)
                    return status.GetError();
            }
            if (entry->interrupt->pending != 0) {
                // A request before entry is handled by the owner without executing
                // an instruction; it cannot disappear in the entering-JIT window.
                entry->current_invocation = ++entry->invocation_counter;
                return FinishRunLocked(thread, *entry, entry->current_invocation, true);
            }
            if (call_args) {
                auto executable = space_.Query(GuestAddress{call_entry.value});
                if (!executable ||
                    !HasPermission(executable.Value().permission, GuestPermission::Execute))
                    return BackendError(ErrorCategory::PermissionDenied, "InvokeGuest",
                                        "entry is not executable");
                auto& state = entry->native->CurrentFrame->State;
                const auto original_rsp = state.gregs[FEXCore::X86State::REG_RSP];
                const std::size_t stack_args = call_args->count > 6 ? call_args->count - 6 : 0;
                const std::uint64_t spill_bytes = stack_args * sizeof(std::uint64_t);
                const auto red_zone = nested ? 128u : 0u;
                if (original_rsp < spill_bytes + 24 + red_zone)
                    return BackendError(ErrorCategory::InvalidArgument, "InvokeGuest",
                                        "stack arithmetic underflow");
                // At function entry RSP % 16 == 8. Argument 7 is [RSP+8], then 8.
                const auto rsp = ((original_rsp - red_zone - spill_bytes) & ~std::uint64_t{15}) - 8;
                const GuestRange frame{GuestAddress{rsp}, original_rsp - rsp};
                auto stack_mapping = space_.Query(frame.base);
                if (!stack_mapping ||
                    HasPermission(stack_mapping.Value().permission, GuestPermission::Execute))
                    return BackendError(ErrorCategory::PermissionDenied, "InvokeGuest",
                                        "requires a non-executable stack");
                auto pin = space_.AcquirePinnedSpan(frame, true);
                if (!pin)
                    return pin.GetError();
                call_stack.emplace(std::move(pin).Value());
                const auto bytes = call_stack->WritableBytes();
                if (bytes.size() > saved_stack_bytes.size())
                    return BackendError(ErrorCategory::InvalidArgument, "InvokeGuest",
                                        "oversized call frame");
                std::memcpy(saved_stack_bytes.data(), bytes.data(), bytes.size());
                std::array<std::byte, 256> frame_bytes{};
                const auto gate = return_gate_.Address();
                std::memcpy(frame_bytes.data(), &gate, sizeof(gate));
                for (std::size_t i = 0; i < stack_args; ++i)
                    std::memcpy(frame_bytes.data() + (i + 1) * 8, &call_args->values[6 + i], 8);
                saved_call_state.emplace();
                RegistersFromCpuState(state, *saved_call_state);
                // All validation/admission precedes the one write; no partial spills.
                std::memcpy(bytes.data(), frame_bytes.data(), bytes.size());
                static constexpr std::array<int, 6> regs{
                    FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI,
                    FEXCore::X86State::REG_RDX, FEXCore::X86State::REG_RCX,
                    FEXCore::X86State::REG_R8,  FEXCore::X86State::REG_R9};
                for (std::size_t i = 0; i < std::min(call_args->count, regs.size()); ++i)
                    state.gregs[regs[i]] = call_args->values[i];
                state.gregs[FEXCore::X86State::REG_RSP] = rsp;
                state.rip = call_entry.value;
                if (call_options.fs_base)
                    state.fs_cached = *call_options.fs_base;
            }
            invocation = ++entry->invocation_counter;
            previous_invocation = entry->current_invocation;
            entry->current_invocation = invocation;
            if (entry->active_runs == 0)
                entry->hle_cancel = std::stop_source{};
            ++entry->active_runs;
            owns_frame = true;
            entry->running = true;
            entry->interrupt->receipt.reset();
            entry->interrupt->stopped_snapshot.reset();
            binding.fex = context_.get();
            binding.native = entry->native;
            binding.fault_page =
                reinterpret_cast<std::uintptr_t>(entry->native->InterruptFaultPage);
            binding.stop_spill = signal_delegator_->GetConfig().ThreadStopHandlerAddressSpillSRA;
            syscall_fault = entry->syscall_fault;
        }
#if defined(GUEST_CPU_TEST_HOOKS)
        // Test-only deterministic owner delay. Running is set, the execution lease is held and every
        // coordinator/context/address-space lock has been released; we are not inside a signal handler
        // and have touched no JIT state yet. Parking here cannot hold a lock QuiesceContext needs, so
        // a test can hold one owner deterministically while a second, genuinely-JIT owner drains and
        // stops independently. On release execution proceeds and the already-pending Pause/Cancel is
        // serviced at the block-entry fault page exactly as for a normally running owner.
        // If the owner did not see a release before the wait budget, do NOT abort with a bare
        // BackendFailure: the controller may already have requested Pause/Cancel (and protected the
        // interrupt fault page) while we were held. Returning early here would leave that page
        // protected and the request without a receipt, so WaitStopped would time out forever. Instead
        // fall through and enter the JIT: the very first block-entry fault page then services the
        // already-pending request through the normal stop path, which restores the page, publishes
        // the stopped snapshot/receipt and releases the lease -- a single, consistent termination.
        // The gate's own timeout result is recorded (TimedOut) for the test, but the Run still
        // completes through the proven stop machinery rather than an ad-hoc failure branch.
        if (test_run_gate_ &&
            !test_run_gate_->WaitAtEntry(context_id_, thread.id, invocation, 5000)) {
            // Gate closed non-cleanly (Aborted before arrival, or the wait budget elapsed with no
            // disposition). The owner must NOT be granted entry to the guest: a timeout cannot be
            // treated as permission to execute. Re-check pending under lock -- the controller may
            // have requested Pause/Cancel/Shutdown while we were held; if so, complete this Run
            // through the normal interrupted finish (page restore, stopped snapshot/receipt, lease
            // release) without running a single guest instruction. With no external request this is
            // a bounded BackendFailure that still restores the interrupt page and clears running, so
            // a later WaitStopped/Destroy cannot hang on a protected page.
            std::lock_guard gate_fail{lock_};
            if (auto* gate_entry = FindOwnedLocked(thread)) {
                if (saved_call_state) {
                    ApplyRegistersToCpuState(*saved_call_state,
                                             gate_entry->native->CurrentFrame->State);
                    const auto bytes = call_stack->WritableBytes();
                    std::memcpy(bytes.data(), saved_stack_bytes.data(), bytes.size());
                }
                if (::mprotect(gate_entry->native->InterruptFaultPage, host_page_size_,
                               PROT_READ | PROT_WRITE) != 0) {
                    gate_entry->interrupt->last_reason = StopReason::BackendFailure;
                    return BackendError(ErrorCategory::BackendFailure, "Run",
                                        "restore interrupt page after gate abort");
                }
                gate_entry->running = false;
                if (gate_entry->interrupt->pending != 0) {
                    // External request already present: service it as the stop reason (zero guest
                    // progress); this consumes only the external request, not an invented internal one.
                    return FinishRunLocked(thread, *gate_entry, invocation, true);
                }
                gate_entry->interrupt->last_reason = StopReason::BackendFailure;
                return BackendError(ErrorCategory::BackendFailure, "Run",
                                    "test entry gate aborted/timed out with no release");
            }
            return BackendError(ErrorCategory::BackendFailure, "Run", "thread disappeared at gate");
        }
#endif
        // The controller may protect the page at any point from claiming the
        // entry above onwards. No tgkill/TID reuse or late unbound signal exists.
        fenv_t host_fp{};
        ::fegetenv(&host_fp);
        bool cooperative_stop = false;
        std::optional<GuestCallResult> nested_stop;
        // Waiting for epoch A to end does not reserve admission: epoch B may
        // start before we reacquire lock_. Retry Busy within one bounded budget,
        // and publish running=true under the coordinator lock with the lease.
        // Never rerun a native HLE or rebuild an InvokeGuest frame on this retry.
        const auto resume_continuation = [&]() -> Result<bool> {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            for (;;) {
                {
                    std::lock_guard guard{lock_};
                    auto* entry = FindOwnedLocked(thread);
                    const auto cancel_mask =
                        (1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                        (1u << static_cast<unsigned>(InterruptReason::Shutdown));
                    if (entry->interrupt->pending & cancel_mask)
                        return false;
                }
                if (config_.resume_internal_drains) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                        return BackendError(ErrorCategory::Timeout, "Run continuation",
                                            "publication admission remained busy");
                    const auto left =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
                            .count();
                    auto ready =
                        space_.WaitForQuiescenceRelease(std::min<std::uint64_t>(left, 20'000'000));
                    if (!ready) {
                        if (ready.GetError().category == ErrorCategory::Timeout)
                            continue;
                        return ready.GetError();
                    }
                }
#if defined(GUEST_CPU_TEST_HOOKS)
                if (!test_continuation_gate_->WaitAtEntry(context_id_, thread.id, invocation, 5000))
                    return BackendError(ErrorCategory::BackendFailure, "Run continuation",
                                        "test continuation gate aborted");
#endif
                {
                    std::lock_guard guard{lock_};
                    auto* entry = FindOwnedLocked(thread);
                    auto admission = space_.AcquireExecutionLease();
                    if (admission) {
                        if (entry->interrupt->pending)
                            return false;
                        auto target =
                            space_.Query(GuestAddress{entry->native->CurrentFrame->State.rip});
                        if (!target ||
                            !HasPermission(target.Value().permission, GuestPermission::Execute))
                            return BackendError(ErrorCategory::PermissionDenied, "Run continuation",
                                                "continuation is no longer executable");
                        lease.Value() = std::move(admission).Value();
                        entry->internally_parked = false;
                        entry->running = true;
                        entry->interrupt->stopped_snapshot.reset();
                        return true;
                    }
                    if (!config_.resume_internal_drains ||
                        admission.GetError().category != ErrorCategory::Busy)
                        return admission.GetError();
#if defined(GUEST_CPU_TEST_HOOKS)
                    test_continuation_retries_.fetch_add(1, std::memory_order_release);
#endif
                }
                // Also bound transient sink exclusion (which need not own a
                // quiescence epoch). No context lock or execution lease is held.
                std::this_thread::yield();
            }
        };
        for (;;) {
            t_binding = &binding;
            binding.hle_pending = false;
            binding.interrupted.store(false, std::memory_order_release);
            // Publish the real owner host FP env to the binding so HandleSyscall can install it
            // around the native call (it runs later, after the guest FPCR has been loaded).
            binding.owner_host_fenv = host_fp;
            binding.owner_host_fenv_valid = true;
            // Writing CPUState.mxcsr alone does not update the executing owner's
            // FPCR. Mirror FEX's SetRoundingMode mapping: x86 down/up are reversed
            // relative to ARM64. Start from guest defaults, not host trap/DN/FZ bits.
            // FEX FillSpecialRegs configures AFP/FIZ from mxcsr when supported.
            const auto mxcsr = binding.native->CurrentFrame->State.mxcsr;
            const std::uint64_t rounding = (mxcsr >> 13) & 3;
            const std::uint64_t guest_fpcr = (((rounding & 1) << 1) | ((rounding & 2) >> 1)) << 22 |
                                             (static_cast<std::uint64_t>((mxcsr >> 15) & 1) << 24);
            asm volatile("msr fpcr, %0\n\tmsr fpsr, xzr" : : "r"(guest_fpcr) : "memory");
            syscall_handler_->RegisterThreadFrame(binding.native->CurrentFrame, syscall_fault);
            // Install the production syscall-fault immediate-exit wrapper for this Run. Save the
            // JIT-installed obj/func so the wrapper can forward to the real handler, and restore
            // them after ExecuteThread. The wrapper runs on this owner thread and reads t_binding
            // for its per-thread stop address/fault flag.
            auto* sys_ptrs = &binding.native->CurrentFrame->Pointers;
            const std::uint64_t saved_syscall_func = sys_ptrs->SyscallHandlerFunc;
            const std::uint64_t saved_syscall_obj = sys_ptrs->SyscallHandlerObj;
            binding.stop_no_spill = signal_delegator_->GetConfig().ThreadStopHandlerAddress;
            binding.syscall_fault_pending.store(false, std::memory_order_release);
            // Seed the per-thread syscall-fault event identity for this Run (N4). The handler fills
            // the fault/operation/PC/category fields if a syscall faults; identity is fixed here so
            // a stale event from a prior Run/invocation can never be reported against the wrong
            // crossing.
            binding.syscall_fault_event = ThreadInterruptBinding::SyscallFaultEvent{};
            binding.syscall_fault_event.context_id = context_id_;
            binding.syscall_fault_event.thread_id = thread.id;
            binding.syscall_fault_event.thread_generation = thread.generation;
            binding.syscall_fault_event.invocation_id = invocation;
            t_syscall_original.obj = reinterpret_cast<void*>(saved_syscall_obj);
            t_syscall_original.func =
                reinterpret_cast<void (*)(void*, FEXCore::Core::CpuStateFrame*)>(
                    saved_syscall_func);
#if defined(GUEST_CPU_TEST_HOOKS)
        t_syscall_trace_enabled = test_syscall_trace_enabled_;
        if (t_syscall_trace_enabled) g_syscall_trace_probe = FexSyscallTraceRecord{};
#endif
        sys_ptrs->SyscallHandlerFunc =
            reinterpret_cast<std::uint64_t>(reinterpret_cast<void*>(&FexSyscallWrapper));
        context_->ExecuteThread(binding.native);
        sys_ptrs->SyscallHandlerFunc = saved_syscall_func;
        sys_ptrs->SyscallHandlerObj = saved_syscall_obj;
        ::fesetenv(&host_fp);
        errno = host_errno.value;
        t_binding = nullptr;
        if (!binding.hle_pending) {
            syscall_handler_->UnregisterThreadFrame(binding.native->CurrentFrame);
            bool park = false;
            if (config_.resume_internal_drains &&
                binding.interrupted.load(std::memory_order_acquire)) {
                std::lock_guard guard{lock_};
                auto* entry = FindOwnedLocked(thread);
                park =
                    std::any_of(drain_tickets_.begin(), drain_tickets_.end(),
                                [&](const auto& ticket) { return ticket.thread_id == thread.id; });
                if (park) {
                    // Already outside ExecuteThread: restore architectural state
                    // from the interrupt snapshot before invalidating any JIT code.
                    const auto flags = context_->ReconstructCompactedEFLAGS(
                        entry->native, true, binding.gprs.data(), binding.pstate);
                    context_->SetFlagsFromCompactedEFLAGS(entry->native, flags);
                    entry->native->CurrentFrame->State.rip = binding.guest_rip;
                    if (::mprotect(entry->native->InterruptFaultPage, host_page_size_,
                                   PROT_READ | PROT_WRITE))
                        return BackendError(ErrorCategory::BackendFailure, "internal drain",
                                            "restore interrupt page", errno);
                    entry->running = false;
                    entry->internally_parked = true;
                    entry->interrupt->stopped_snapshot =
                        CaptureSnapshot(thread, *entry, SnapshotKind::SafePoint);
                    stopped_changed_.notify_all();
                }
            }
            if (!park)
                break;
            binding.interrupted.store(false, std::memory_order_release);
            call_stack.reset();
            lease.Value() = ExecutionLease{};
            auto resumed = resume_continuation();
            if (!resumed)
                return resumed.GetError();
            if (!resumed.Value()) {
                cooperative_stop = true;
                break;
            }
            continue;
        }

        std::stop_token cancel;
        {
            std::lock_guard guard{lock_};
            auto* entry = FindOwnedLocked(thread);
            entry->running = false;
            entry->interrupt->stopped_snapshot =
                CaptureSnapshot(thread, *entry, SnapshotKind::HleBoundary);
            cancel = entry->hle_cancel.get_token();
        }
        // No JIT frame is parked here. Release the call-frame pin before the
        // execution lease, so a VM HLE can enter a real coordinated transaction.
        call_stack.reset();
        lease.Value() = ExecutionLease{};
        std::optional<std::uint64_t> thread_exit;
        {
            Hle::HleScope scope{*this, thread, invocation, cancel};
            syscall_handler_->DispatchNative(binding.native->CurrentFrame, &binding, true);
            nested_stop = scope.FailedCallback();
            thread_exit = scope.ThreadExitResult();
        }
        syscall_handler_->UnregisterThreadFrame(binding.native->CurrentFrame);
        ::fesetenv(&host_fp);
        if (binding.syscall_fault_pending.load(std::memory_order_acquire))
            break;
        if (nested_stop && (nested_stop->reason == StopReason::GuestFault ||
                            nested_stop->reason == StopReason::BackendFailure))
            break;
        if (thread_exit) {
            binding.native->CurrentFrame->State.rip = return_gate_.Address();
            binding.native->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = *thread_exit;
            break;
        }
        // x86 SYSCALL saved its successor in RCX. Native marshaling restores the
        // architectural RCX; start a fresh translation there, not in old JIT code.
        binding.native->CurrentFrame->State.rip =
            binding.native->CurrentFrame->State.gregs[FEXCore::X86State::REG_RCX];
        auto resumed = resume_continuation();
        if (!resumed)
            return resumed.GetError();
        if (!resumed.Value()) {
            cooperative_stop = true;
            break;
        }
        }

        std::lock_guard guard{lock_};
        auto *entry = FindOwnedLocked(thread);
        if (!entry)
            return BackendError(ErrorCategory::BackendFailure, "Run", "thread disappeared");
        entry->running = false;
        if (::mprotect(entry->native->InterruptFaultPage, host_page_size_,
                       PROT_READ | PROT_WRITE) != 0) {
            entry->interrupt->last_reason = StopReason::BackendFailure;
            return BackendError(ErrorCategory::BackendFailure, "Run", "restore interrupt page",
                                errno);
        }
        const bool interrupted = binding.interrupted.load(std::memory_order_acquire);
        // Publish the structured syscall-fault event (if any) to the entry for BuildRunResultLocked.
        entry->last_syscall_fault_event = binding.syscall_fault_event;
        if (interrupted) {
            const auto flags = context_->ReconstructCompactedEFLAGS(
                entry->native, true, binding.gprs.data(), binding.pstate);
            context_->SetFlagsFromCompactedEFLAGS(entry->native, flags);
            entry->native->CurrentFrame->State.rip = binding.guest_rip;
        }
        auto result = FinishRunLocked(thread, *entry, invocation, interrupted || cooperative_stop);
        if (nested_stop && result &&
            (nested_stop->reason == StopReason::GuestFault ||
             nested_stop->reason == StopReason::BackendFailure)) {
            result.Value().primary_reason = nested_stop->reason;
            result.Value().pending_reasons |= nested_stop->pending_reasons;
            result.Value().fault = nested_stop->fault;
            result.Value().snapshot = nested_stop->snapshot;
            entry->interrupt->last_reason = nested_stop->reason;
            entry->interrupt->stopped_snapshot = nested_stop->snapshot;
        }
        if (saved_call_state && result && result.Value().primary_reason == StopReason::Returned) {
            auto& state = entry->native->CurrentFrame->State;
            state.gregs[FEXCore::X86State::REG_RSP] = saved_call_state->Rsp();
            state.rip = saved_call_state->rip;
            if (call_options.fs_base)
                state.fs_cached = saved_call_state->fs_base;
            // Return the gate snapshot to the call's owner, but publish the restored
            // caller continuation before releasing the lock to controllers.
            entry->interrupt->stopped_snapshot =
                CaptureSnapshot(thread, *entry, SnapshotKind::SafePoint);
            PublishReceiptLocked(thread, *entry);
        }
        return result;
    }

    [[nodiscard]] Result<GuestCallResult> InvokeGuest(ThreadHandle thread, GuestCodeAddress entry,
                                                      const GuestCallArgs &args,
                                                      const GuestCallOptions &options) override {
        if (args.count > GuestCallArgs::kMaxArguments) {
            return BackendError(ErrorCategory::InvalidArgument, "InvokeGuest",
                                "too many guest call arguments");
        }
        auto* scope = Hle::HleScope::Current();
        const bool nested = scope && scope->CallbackAdmission() && &scope->Context() == this &&
                            scope->Thread() == thread;
        std::optional<CpuSnapshot> caller;
        std::uint64_t returning_stack{}, callret_stack{};
        StopReason previous_reason{};
        if (nested) {
            std::lock_guard guard{lock_};
            auto* owner = FindOwnedLocked(thread);
            if (!owner || owner->running || owner->active_runs == 0)
                return BackendError(ErrorCategory::WrongState, "InvokeGuest", "no live HLE caller");
            caller = CaptureSnapshot(thread, *owner, SnapshotKind::HleBoundary);
            returning_stack = owner->native->CurrentFrame->ReturningStackLocation;
            callret_stack = owner->native->CurrentFrame->State.callret_sp;
            previous_reason = owner->interrupt->last_reason;
        }
        auto restore_caller = [&](void*) {
            if (!caller)
                return;
            std::lock_guard guard{lock_};
            if (auto* owner = FindOwnedLocked(thread)) {
                ApplyRegistersToCpuState(caller->registers, owner->native->CurrentFrame->State);
                context_->SetFlagsFromCompactedEFLAGS(
                    owner->native, static_cast<std::uint32_t>(caller->registers.rflags));
                owner->native->CurrentFrame->ReturningStackLocation = returning_stack;
                owner->native->CurrentFrame->State.callret_sp = callret_stack;
                owner->current_invocation = caller->invocation_id;
                owner->interrupt->last_reason = previous_reason;
                owner->interrupt->stopped_snapshot =
                    CaptureSnapshot(thread, *owner, SnapshotKind::HleBoundary);
            }
        };
        std::unique_ptr<void, decltype(restore_caller)> continuation{reinterpret_cast<void*>(1),
                                                                     restore_caller};
        auto run = RunInternal(thread, RunOptions{}, &args, entry, options);
        if (!run) {
            return run.GetError();
        }
        const RunResult &r = run.Value();
        GuestCallResult call{};
        call.reason = r.primary_reason;
        call.pending_reasons = r.pending_reasons;
        call.invocation_id = r.invocation_id;
        call.stop_epoch = r.stop_epoch;
        call.snapshot = r.snapshot;
        call.fault = r.fault;
        if (r.primary_reason == StopReason::Returned) {
            call.return_value = r.snapshot.registers.Get(Gpr::Rax);
        }
        return call;
    }

    [[nodiscard]] Result<RunResult> Step(ThreadHandle thread, const StepOptions &) override {
        std::lock_guard guard{lock_};
        if (FindOwnedLocked(thread) == nullptr) {
            return OwnershipError(thread, "Step");
        }
        // Single-instruction stepping needs a JIT block-length limit that this
        // backend does not yet drive. Refusing before execution leaves guest
        // state untouched, which T07 requires; returning a whole block as if it
        // were one instruction would be worse than refusing.
        return BackendError(ErrorCategory::Unsupported, "Step",
                            "single-instruction step is not implemented by this backend yet");
    }

    [[nodiscard]] Result<CpuSnapshot> ReadRegisters(ThreadHandle thread) const override {
        std::lock_guard guard{lock_};
        const ThreadEntry *entry = Find(thread);
        if (entry == nullptr) {
            return BackendError(ErrorCategory::InvalidHandle, "ReadRegisters",
                                "no live thread with this id and generation");
        }
        if (entry->running) {
            // Reading while the JIT owns the registers would report stale
            // memory as if it were current state (D03).
            return BackendError(ErrorCategory::AlreadyRunning, "ReadRegisters",
                                "thread is executing; registers are not at a safe point");
        }
        if (!entry->interrupt->stopped_snapshot)
            return BackendError(ErrorCategory::WrongState, "ReadRegisters",
                                "no published snapshot");
        return *entry->interrupt->stopped_snapshot;
    }

    [[nodiscard]] Result<void> WriteRegisters(ThreadHandle thread, const RegisterPatch &patch,
                                              std::uint64_t stop_epoch) override {
        std::lock_guard guard{lock_};
        auto *entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            return OwnershipError(thread, "WriteRegisters");
        }
        if (entry->running || entry->active_runs != 0) {
            return BackendError(ErrorCategory::AlreadyRunning, "WriteRegisters",
                                "cannot write registers of an executing thread");
        }
        if (entry->interrupt->last_reason == StopReason::GuestFault ||
            entry->interrupt->last_reason == StopReason::BackendFailure)
            return BackendError(ErrorCategory::WrongState, "WriteRegisters",
                                "unrecoverable fault state");
        if (stop_epoch != entry->stop_epoch) {
            return BackendError(ErrorCategory::StaleEpoch, "WriteRegisters",
                                "the supplied stop epoch is not the thread's current one");
        }

        ApplyPatchToState(patch, entry->native->CurrentFrame->State);
        ApplyXmmPatch(patch, entry->native);
        ++entry->stop_epoch;
        entry->interrupt->stopped_snapshot =
            CaptureSnapshot(thread, *entry, SnapshotKind::SafePoint);
        PublishReceiptLocked(thread, *entry);
        return Result<void>{};
    }

    [[nodiscard]] Result<void> DestroyThread(ThreadHandle thread) override {
        std::lock_guard guard{lock_};
        if (space_.IsQuiescent())
            return BackendError(ErrorCategory::Busy, "DestroyThread", "coordinated drain is active");
        auto *entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            return OwnershipError(thread, "DestroyThread");
        }
        if (entry->running || entry->active_runs != 0) {
            return BackendError(ErrorCategory::AlreadyRunning, "DestroyThread",
                                "cannot destroy a thread that is executing");
        }

        context_->DestroyThread(entry->native);
        threads_.erase(thread.id);
        stopped_changed_.notify_all();
        return Result<void>{};
    }

    [[nodiscard]] std::size_t LiveThreadCount() const override {
        std::lock_guard guard{lock_};
        return threads_.size();
    }

    // --- asynchronous control ---------------------------------------------------------------

    [[nodiscard]] std::uint64_t ContextId() const noexcept override { return context_id_; }

    // Backend-internal: install a typed native HLE function and get the guest operation number to
    // place in rax before the syscall. Exposed via the fex backend header, not the backend-free API.
    template <typename Function>
    [[nodiscard]] Result<std::uint64_t> RegisterHle(Function function, std::string name) {
        return syscall_handler_->Registry().Register(function, std::move(name));
    }

    [[nodiscard]] Hle::HleCallRegistry* HleRegistryPointer() { return &syscall_handler_->Registry(); }

#if defined(GUEST_CPU_TEST_HOOKS)
    [[nodiscard]] FexTestRunGate* TestRunGatePointer() {
        return test_run_gate_.get();
    }
    [[nodiscard]] FexTestRunGate* TestContinuationGatePointer() {
        return test_continuation_gate_.get();
    }
    std::uint64_t TestContinuationRetries() const {
        return test_continuation_retries_.load(std::memory_order_acquire);
    }
    void SetSyscallTraceEnabled(bool trace) { test_syscall_trace_enabled_ = trace; }
#endif

    [[nodiscard]] Result<QuiescenceToken> QuiesceContext(std::uint64_t timeout_ns) override {
        std::unique_lock coordinator{coordinator_lock_, std::try_to_lock};
        if (!coordinator.owns_lock())
            return BackendError(ErrorCategory::Busy, "QuiesceContext", "another coordinator is active");
        const auto budget = std::min<std::uint64_t>(timeout_ns ? timeout_ns : 1'000'000'000,
                                                   60'000'000'000);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(budget);
        auto remaining = [&]() -> std::uint64_t {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            return ns > 0 ? ns : 0;
        };
        std::size_t stopped_count{};
        std::vector<std::stop_source> hle_wakes;
        {

            std::lock_guard guard{lock_};

            if (!drain_) {

                auto admission = space_.BeginDrain();

                if (!admission) return admission.GetError();
                drain_.emplace(std::move(admission).Value());
            }
            stopped_count = threads_.size();
            // Issue ALL stop requests before waiting. A retry keeps its original tickets.
            for (auto& [id, entry] : threads_) {
                if (!entry.running && (entry.active_runs == 0 || config_.resume_internal_drains))
                    continue;
                if (auto* scope = Hle::HleScope::Current();
                    scope && &scope->Context() == this &&
                    scope->Thread() == ThreadHandle{id, entry.generation})
                    continue;
                const bool requested = std::any_of(drain_tickets_.begin(), drain_tickets_.end(),
                    [&](const auto& t) { return t.thread_id == id; });
                if (requested) continue;
                auto ticket = RequestInterruptLocked({id, entry.generation}, InterruptReason::Pause);
                if (!ticket) return ticket.GetError();
                drain_tickets_.push_back(ticket.Value());
                // Production publication parks only JIT execution. A native HLE
                // boundary already has no execution lease; poisoning its persistent
                // cancellation source would cancel all future HLEs in this Run.
                if (!config_.resume_internal_drains)
                    hle_wakes.push_back(entry.hle_cancel);
            }
        }

        for (auto& wake : hle_wakes)
            wake.request_stop();

        for (const auto& ticket : drain_tickets_) {
            const auto left = remaining();
            if (!left)
                return BackendError(ErrorCategory::Timeout, "QuiesceContext", "drain deadline expired; retry to recover");
            if (config_.resume_internal_drains) {
                std::unique_lock guard{lock_};
                if (!stopped_changed_.wait_for(guard, std::chrono::nanoseconds(left), [&] {
                        auto* entry = Find({ticket.thread_id, ticket.thread_generation});
                        // HLE return and nested entry must reacquire admission
                        // under lock_; a live frame outside JIT is quiescent too.
                        return !entry || !entry->running;
                    })) {
                    return BackendError(ErrorCategory::Timeout, "QuiesceContext",
                                        "owner has not left JIT");
                }
            } else if (auto receipt = WaitStopped(ticket, left); !receipt)
                return receipt.GetError();
        }

        auto token = space_.FinishDrain(*drain_, remaining(), stopped_count);

        if (!token) return token.GetError(); // Retain admission on every failure.
        {

            std::lock_guard guard{lock_};

            for (const auto& ticket : drain_tickets_) {
                auto* entry = Find({ticket.thread_id, ticket.thread_generation});
                if (!entry) continue;
                auto& state = *entry->interrupt;
                state.requests.erase(ticket.epoch); // Consume only our own Pause.
                state.pending = 0;
                for (const auto& [epoch, reason] : state.requests)
                    state.pending |= 1u << static_cast<std::uint32_t>(reason);
                // An internal ticket may be newer than a user's Cancel. Retiring it must
                // leave that user's latest ticket resumable, using the same frozen snapshot.
                state.request_epoch = state.requests.empty() ? 0 : state.requests.rbegin()->first;
                state.acked_epoch = state.request_epoch;
                if (state.requests.empty()) state.receipt.reset();
                else PublishReceiptLocked({ticket.thread_id, ticket.thread_generation}, *entry);
            }
            drain_tickets_.clear();
            drain_.reset(); // Reservation was moved into the successful token.
        }
        return token;
    }

    [[nodiscard]] Result<void> ClearCodeCache(const QuiescenceToken& token) override {
        // Route through the memory transaction guard (identity, epoch, callback exclusion and
        // poison recovery). FullFlush covers historical RW/unmapped ranges and the host gate.
        return space_.InvalidateCode(token, {space_.ReservationBase(), space_.ReservationSize()},
                                     InvalidationReason::FullFlush);
    }

    [[nodiscard]] Result<InterruptTicket> RequestInterrupt(ThreadHandle thread,
                                                           InterruptReason reason) override {
        std::unique_lock guard{lock_};
        auto ticket = RequestInterruptLocked(thread, reason);
        std::optional<std::stop_source> wake;
        if (ticket)
            wake = Find(thread)->hle_cancel;
        guard.unlock();
        if (wake)
            wake->request_stop(); // callbacks never run with the context lock held
        return ticket;
    }

  private:
    [[nodiscard]] Result<InterruptTicket> RequestInterruptLocked(ThreadHandle thread,
                                                                InterruptReason reason) {
        if (reason != InterruptReason::Pause && reason != InterruptReason::Cancel &&
            reason != InterruptReason::Shutdown)
            return BackendError(ErrorCategory::InvalidArgument, "RequestInterrupt",
                                "unknown reason");
        auto *entry = Find(thread);
        if (!entry)
            return BackendError(ErrorCategory::InvalidHandle, "RequestInterrupt", "stale handle");
        if (entry->running &&
            ::mprotect(entry->native->InterruptFaultPage, host_page_size_, PROT_NONE) != 0)
            return BackendError(ErrorCategory::BackendFailure, "RequestInterrupt",
                                "arm interrupt page", errno);
        auto &state = *entry->interrupt;
        const auto epoch = ++next_request_epoch_;
        state.requests.emplace(epoch, reason);
        state.request_epoch = epoch;
        state.pending |= 1u << static_cast<std::uint32_t>(reason);
        // Already stopped: a new request is covered by the existing owner-published
        // snapshot. Reuse its stop epoch; never re-read state or fabricate a stop.
        if (!entry->running && entry->active_runs == 0 && state.stopped_snapshot)
            PublishReceiptLocked(thread, *entry);
        return InterruptTicket{context_id_, thread.id, thread.generation, epoch, reason};
    }

  public:
    [[nodiscard]] Result<StopReceipt> WaitStopped(const InterruptTicket &ticket,
                                                  std::uint64_t timeout_ns) override {
        if (!ticket.IsValid() || ticket.context_id != context_id_)
            return BackendError(ErrorCategory::InvalidArgument, "WaitStopped",
                                "wrong context ticket");
        std::unique_lock guard{lock_};
        const ThreadHandle handle{ticket.thread_id, ticket.thread_generation};
        auto valid = [&]() -> bool {
            auto *entry = Find(handle);
            if (!entry)
                return false;
            const auto &requests = entry->interrupt->requests;
            auto request = requests.find(ticket.epoch);
            return request != requests.end() && request->second == ticket.reason;
        };
        if (!valid())
            return BackendError(ErrorCategory::InvalidHandle, "WaitStopped",
                                "stale or unknown ticket");
        if (auto* entry = Find(handle); (entry->running || entry->active_runs != 0) &&
                                        entry->owner == std::this_thread::get_id())
            return BackendError(ErrorCategory::WrongThread, "WaitStopped",
                                "owner cannot wait for itself");
        const auto bounded_timeout =
            std::min<std::uint64_t>(timeout_ns ? timeout_ns : 1'000'000'000, 60'000'000'000);
        if (!stopped_changed_.wait_for(guard, std::chrono::nanoseconds(bounded_timeout), [&] {
                if (!valid())
                    return true;
                auto *entry = Find(handle);
                return !entry->running && entry->active_runs == 0 && entry->interrupt->receipt &&
                       entry->interrupt->acked_epoch >= ticket.epoch;
            }))
            return BackendError(ErrorCategory::Timeout, "WaitStopped", "owner has not stopped");
        if (!valid())
            return BackendError(ErrorCategory::StaleEpoch, "WaitStopped",
                                "ticket retired while waiting");
        auto result = *Find(handle)->interrupt->receipt;
        result.request_epoch = ticket.epoch;
        return result;
    }

    [[nodiscard]] Result<void> Resume(ThreadHandle thread,
                                      std::uint64_t acknowledged_epoch) override {
        std::lock_guard guard{lock_};
        // A coordinated transaction pauses owners on its own behalf. The space's quiescence closes
        // new execution; keep Resume from reopening an owner while memory is being changed.
        if (space_.IsQuiescent()) {
            return BackendError(ErrorCategory::Busy, "Resume",
                                "a coordinated quiescence holds this context");
        }
        auto *entry = Find(thread);
        if (!entry)
            return BackendError(ErrorCategory::InvalidHandle, "Resume", "stale handle");
        return ConsumeResumeLocked(*entry, acknowledged_epoch);
    }

    [[nodiscard]] Result<void> InvalidateCode(const QuiescenceToken &token, GuestRange range,
                                              InvalidationReason reason) override {
        return space_.InvalidateCode(token, range, reason);
    }

    // --- CodeInvalidationSink ---------------------------------------------------
    //
    // GuestAddressSpace::PublishCode / InvalidateCode reach the same code below. Before this
    // existed the public memory API only bumped a generation, so a caller could publish new bytes,
    // see both calls succeed, and still execute the previous translation -- reproduced as P1-C in
    // the 2026-09-08 publication review. The address space has already checked the token by the
    // time it calls this, so there is no token argument to re-check here.

    [[nodiscard]] std::string_view Name() const override { return "FEXCore"; }

    [[nodiscard]] Status DiscardTranslations(GuestRange range, InvalidationReason reason) override {
        return DiscardTranslationsImpl(range, reason, "DiscardTranslations");
    }

  private:
    [[nodiscard]] Status DiscardTranslationsImpl(GuestRange range, InvalidationReason reason,
                                                 const char *operation) {
        std::lock_guard guard{lock_};
        for (auto &[id, entry] : threads_) {
            if (entry.running) {
                return BackendError(ErrorCategory::AlreadyRunning, operation,
                                    "a guest thread is still executing");
            }
        }

        auto checked = GuestRange::Checked(range.base, range.size);
        if (!checked) {
            return checked.GetError();
        }

        // FEX requires this exclusive lock for BOTH shared and per-owner cache retirement.
        // InvalidateRange walks guest CodePages keys, not host memory; the entire owned guest
        // reservation also covers mappings removed or temporarily made non-executable.
        std::scoped_lock code_guard{context_->GetCodeInvalidationMutex()};
        auto discard = [&](GuestRange affected) {
            context_->InvalidateCodeBuffersCodeRange(affected.base.value, affected.size);
            for (auto& [id, entry] : threads_)
                if (entry.native)
                    context_->InvalidateThreadCachedCodeRange(entry.native, affected.base.value,
                                                              affected.size);
        };
        discard(range);
        if (reason == InvalidationReason::FullFlush)
            discard({GuestAddress{return_gate_.Address()}, return_gate_.Size()});
        return Result<void>{};
    }

  public:
    [[nodiscard]] std::uint64_t ReturnGateAddress() const noexcept {
        return return_gate_.Address();
    }

  private:
    struct ThreadEntry final {
        FEXCore::Core::InternalThreadState *native{};
        std::uint64_t generation{};
        std::thread::id owner{};
        std::uint64_t guest_tid{};
        std::uint64_t entry_rip{};
        std::uint64_t invocation_counter{};
        std::uint64_t current_invocation{};
        std::uint64_t stop_epoch{1};
        bool running{false};
        bool internally_parked{false};
        unsigned active_runs{};
        std::stop_source hle_cancel{};
        // Stable address, so the signal handler can hold a pointer to it while the map changes.
        std::shared_ptr<InterruptState> interrupt{std::make_shared<InterruptState>()};

        // Set when this specific guest thread executes a syscall with no HLE path. Attributed per
        // thread (via the frame) so one owner's unregistered-entry fault is never consumed by a
        // different owner's Run (R2-H05).
        std::shared_ptr<std::atomic<bool>> syscall_fault{std::make_shared<std::atomic<bool>>(false)};

        // Structured syscall-fault event from the most recent Run (N4). Published by Run after
        // ExecuteThread (copied from the owner binding) and consumed by BuildRunResultLocked to fill
        // GuestFaultInfo attribution; reset at the start of each Run so a new Run/invocation never
        // reports the previous crossing's fault.
        ThreadInterruptBinding::SyscallFaultEvent last_syscall_fault_event{};

        // The guest descriptor table. CPUState only holds a pointer to it, and
        // the decoder dereferences that pointer on the very first block to read
        // CS.L and decide 64-bit mode -- so this must exist before any code
        // runs and must outlive the FEX thread. Owned per thread rather than
        // per context so one thread's segment state cannot disturb another's.
        std::unique_ptr<std::array<FEXCore::Core::CPUState::gdt_segment, 32>> gdt;

        // Also embedder-owned and also referenced by raw pointer from CPUState,
        // so it has the same lifetime requirement as the GDT.
        std::unique_ptr<CallRetStack> callret;
    };

    [[nodiscard]] Status ConsumeResumeLocked(ThreadEntry &entry, std::uint64_t epoch) {
        auto &state = *entry.interrupt;
        if (entry.running || entry.active_runs != 0 || !state.receipt || epoch == 0 ||
            epoch != state.acked_epoch || epoch != state.request_epoch)
            return BackendError(ErrorCategory::StaleEpoch, "Resume",
                                "resume must name the current stopped request epoch");
        if (state.last_reason == StopReason::GuestFault ||
            state.last_reason == StopReason::BackendFailure)
            return BackendError(ErrorCategory::WrongState, "Resume",
                                "faulted execution cannot be resumed");
        state.pending = 0;
        state.requests.clear();
        state.receipt.reset();
        stopped_changed_.notify_all();
        return Ok();
    }

    void PublishReceiptLocked(ThreadHandle handle, ThreadEntry &entry) {
        auto &state = *entry.interrupt;
        if (!state.stopped_snapshot || state.pending == 0)
            return;
        const auto cancel = (1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                            (1u << static_cast<unsigned>(InterruptReason::Shutdown));
        auto reason = (state.pending & cancel) ? StopReason::Cancelled : StopReason::PauseRequested;
        if (state.last_reason == StopReason::GuestFault ||
            state.last_reason == StopReason::BackendFailure)
            reason = state.last_reason;
        state.acked_epoch = state.request_epoch;
        state.receipt =
            StopReceipt{context_id_, handle.id,     state.request_epoch,    entry.stop_epoch,
                        reason,      state.pending, *state.stopped_snapshot};
        stopped_changed_.notify_all();
    }

    Result<RunResult> FinishRunLocked(ThreadHandle handle, ThreadEntry &entry,
                                      std::uint64_t invocation, bool interrupted) {
        auto result = BuildRunResultLocked(handle, entry, invocation, std::nullopt);
        if (interrupted) {
            const auto cancel = (1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                                (1u << static_cast<unsigned>(InterruptReason::Shutdown));
            auto &value = result.Value();
            value.primary_reason = (entry.interrupt->pending & cancel) ? StopReason::Cancelled
                                                                       : StopReason::PauseRequested;
            value.pending_reasons = BitOf(value.primary_reason);
            if (entry.interrupt->pending & (1u << static_cast<unsigned>(InterruptReason::Pause)))
                value.pending_reasons |= BitOf(StopReason::PauseRequested);
            value.guest_pc = value.snapshot.registers.rip;
            value.snapshot.kind = SnapshotKind::SafePoint;
            value.fault.reset();
        }
        if (entry.interrupt->pending & (1u << static_cast<unsigned>(InterruptReason::Pause)))
            result.Value().pending_reasons |= BitOf(StopReason::PauseRequested);
        if (entry.interrupt->pending & ((1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                                        (1u << static_cast<unsigned>(InterruptReason::Shutdown))))
            result.Value().pending_reasons |= BitOf(StopReason::Cancelled);
        result.Value().primary_reason = SelectPrimaryReason(result.Value().pending_reasons);
        entry.interrupt->last_reason = result.Value().primary_reason;
        entry.interrupt->stopped_snapshot = result.Value().snapshot;
        PublishReceiptLocked(handle, entry);
        return result;
    }

    // Points CPUState at this thread's GDT and installs a flat 64-bit code
    // segment. Without it FEXCore::Frontend::Decoder dereferences a null
    // segment_arrays entry as soon as it compiles the first block.
    static void InitializeSegments(FEXCore::Core::CPUState &state,
                                   std::array<FEXCore::Core::CPUState::gdt_segment, 32> &gdt) {
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt.data();
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt.data();
        state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;

        auto *code_segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
        FEXCore::Core::CPUState::SetGDTBase(code_segment, 0);
        FEXCore::Core::CPUState::SetGDTLimit(code_segment, 0xF'FFFFU);
        state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*code_segment);
        // L=1, D=0 is 64-bit mode. The decoder asserts this matches the
        // context's Is64BitMode, so a mismatch fails loudly rather than
        // decoding 32-bit instructions from 64-bit code.
        code_segment->L = 1;
        code_segment->D = 0;
    }

    [[nodiscard]] ThreadEntry *Find(ThreadHandle handle) {
        auto found = threads_.find(handle.id);
        return found != threads_.end() && found->second.generation == handle.generation
                   ? &found->second
                   : nullptr;
    }

    [[nodiscard]] const ThreadEntry *Find(ThreadHandle handle) const {
        auto it = threads_.find(handle.id);
        if (it == threads_.end() || it->second.generation != handle.generation) {
            return nullptr;
        }
        return &it->second;
    }

    // Resolves a handle and enforces that the caller owns it. Owner-thread checking is what keeps
    // two host threads from driving one guest thread.
    //
    // Requires lock_ to already be held, and the returned pointer is only valid while the caller
    // keeps holding it. An earlier version took the lock inside a helper and returned this pointer
    // to a caller that then used it unlocked, which is the window the 2026-09-08 review recorded as
    // part of R5.
    [[nodiscard]] ThreadEntry *FindOwnedLocked(ThreadHandle handle) {
        auto it = threads_.find(handle.id);
        if (it == threads_.end() || it->second.generation != handle.generation) {
            return nullptr;
        }
        if (it->second.owner != std::this_thread::get_id()) {
            return nullptr;
        }
        return &it->second;
    }

    // Distinguishes "no such thread" from "not your thread" for the caller, which FindOwnedLocked
    // deliberately collapses so it can return a single pointer.
    [[nodiscard]] Error OwnershipError(ThreadHandle handle, std::string_view operation) {
        auto it = threads_.find(handle.id);
        if (it == threads_.end() || it->second.generation != handle.generation) {
            return BackendError(ErrorCategory::InvalidHandle, operation,
                                "no live thread with this id and generation");
        }
        return BackendError(ErrorCategory::WrongThread, operation,
                            "only the creating thread may drive this guest thread");
    }

    void ApplyPatchToState(const RegisterPatch &patch, FEXCore::Core::CPUState &state) const {
        if (HasAll(patch.fields, RegisterValidity::Gpr)) {
            for (std::size_t i = 0; i < kGprCount; ++i) {
                if ((patch.gpr_mask & (1u << i)) != 0) {
                    state.gregs[i] = patch.values.gpr[i];
                }
            }
        }
        if (HasAll(patch.fields, RegisterValidity::Rip)) {
            state.rip = patch.values.rip;
        }
        if (HasAll(patch.fields, RegisterValidity::Mxcsr)) {
            state.mxcsr = patch.values.mxcsr;
        }
        if (HasAll(patch.fields, RegisterValidity::SegmentBases)) {
            state.fs_cached = patch.values.fs_base;
            state.gs_cached = patch.values.gs_base;
        }
    }

    // RFLAGS and XMM go through context calls rather than raw CPUState fields:
    // FEX stores flags decomposed across a byte array and may hold XMM in an
    // AVX layout, so writing the struct directly would corrupt them.
    void ApplyXmmPatch(const RegisterPatch &patch, FEXCore::Core::InternalThreadState *native) {
        if (HasAll(patch.fields, RegisterValidity::Rflags)) {
            context_->SetFlagsFromCompactedEFLAGS(native,
                                                  static_cast<std::uint32_t>(patch.values.rflags));
        }
        if (!HasAll(patch.fields, RegisterValidity::Xmm)) {
            return;
        }

        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> low{};
        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> high{};
        // Read-modify-write: the mask selects individual registers, so the
        // unselected ones must keep their current values.
        context_->ReconstructXMMRegisters(native, low.data(),
                                          host_features_.SupportsAVX ? high.data() : nullptr);
        for (std::size_t i = 0; i < kXmmCount && i < low.size(); ++i) {
            if ((patch.xmm_mask & (1u << i)) != 0) {
                // Xmm is two explicit halves rather than a byte array, so build
                // the 128-bit value from them instead of memcpy'ing a struct
                // whose layout could drift.
                low[i] = (static_cast<__uint128_t>(patch.values.xmm[i].high) << 64) |
                         patch.values.xmm[i].low;
            }
        }
        context_->SetXMMRegistersFromState(native, low.data(),
                                           host_features_.SupportsAVX ? high.data() : nullptr);
    }

    [[nodiscard]] CpuSnapshot CaptureSnapshot(ThreadHandle handle, const ThreadEntry &entry,
                                              SnapshotKind kind) const {
        CpuSnapshot snapshot{};
        snapshot.thread_id = handle.id;
        snapshot.thread_generation = handle.generation;
        snapshot.stop_epoch = entry.stop_epoch;
        snapshot.invocation_id = entry.current_invocation;
        snapshot.kind = kind;
        snapshot.mapping_generation = space_.MappingGeneration();
        snapshot.code_generation = space_.CodeGeneration();

        const auto &state = entry.native->CurrentFrame->State;
        for (std::size_t i = 0; i < kGprCount; ++i) {
            snapshot.registers.gpr[i] = state.gregs[i];
        }
        snapshot.registers.rip = state.rip;
        snapshot.registers.mxcsr = state.mxcsr;
        snapshot.registers.fs_base = state.fs_cached;
        snapshot.registers.gs_base = state.gs_cached;

        // WasInJIT=false: this is a safe point, so flags are reconstructed from
        // the published state rather than from host registers.
        snapshot.registers.rflags =
            context_->ReconstructCompactedEFLAGS(entry.native, /*WasInJIT=*/false, nullptr, 0);

        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> low{};
        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> high{};
        context_->ReconstructXMMRegisters(entry.native, low.data(),
                                          host_features_.SupportsAVX ? high.data() : nullptr);
        for (std::size_t i = 0; i < kXmmCount && i < low.size(); ++i) {
            snapshot.registers.xmm[i].low = static_cast<std::uint64_t>(low[i]);
            snapshot.registers.xmm[i].high = static_cast<std::uint64_t>(low[i] >> 64);
        }

        // Only claim the fields actually captured. AVX high state is not
        // reported, so it stays out of the validity mask rather than appearing
        // as valid zeros.
        snapshot.registers.validity = RegisterValidity::Gpr | RegisterValidity::Rip |
                                      RegisterValidity::Rflags | RegisterValidity::Xmm |
                                      RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
        return snapshot;
    }

    // Requires lock_ to be held: it reads and advances the entry's stop epoch.
    [[nodiscard]] Result<RunResult> BuildRunResultLocked(ThreadHandle handle, ThreadEntry &entry,
                                                         std::uint64_t invocation,
                                                         std::optional<StepInfo> step) {
        entry.stop_epoch++;

        RunResult result{};
        result.thread_id = handle.id;
        result.thread_generation = handle.generation;
        result.invocation_id = invocation;
        result.stop_epoch = entry.stop_epoch;
        result.step = step;

        const std::uint64_t rip = entry.native->CurrentFrame->State.rip;
        const std::uint64_t gate = return_gate_.Address();

        if (entry.syscall_fault->exchange(false, std::memory_order_acq_rel) ||
            syscall_handler_->TakeUnknownThreadSyscallGlobal()) {
            // This guest thread executed a syscall with no HLE path: a fault attributed to it, not
            // a context-wide boolean another owner could consume. Fill the structured event (N4)
            // so the caller sees the exact operation/crossing that faulted.
            result.primary_reason = StopReason::GuestFault;
            result.pending_reasons |= BitOf(StopReason::GuestFault);
            GuestFaultInfo fault{};
            const auto& ev = entry.last_syscall_fault_event;
            fault.guest_rip = ev.present ? ev.fault_guest_rip : rip;
            fault.access = GuestAccessKind::Execute;
            fault.recoverable = false;
            if (ev.present) {
                fault.syscall_operation = ev.operation;
                fault.context_id = ev.context_id;
                fault.thread_generation = ev.thread_generation;
                fault.invocation_id = ev.invocation_id;
                fault.syscall_category = ev.category;
                if (ev.has_error)
                    fault.syscall_errno = ev.system_error;
            }
            result.fault = fault;
        } else if (rip >= gate && rip < gate + return_gate_.Size()) {
            // Stopped at the registered gate: this is the only RIP that counts
            // as a normal return (D05).
            result.primary_reason = StopReason::Returned;
            result.pending_reasons |= BitOf(StopReason::Returned);
            result.guest_pc = rip;
        } else {
            // Execution left the JIT somewhere unregistered -- a real HLT, an
            // illegal instruction, or a fault. Reporting Returned here would be
            // exactly the conflation D05 forbids.
            result.primary_reason = StopReason::GuestFault;
            result.pending_reasons |= BitOf(StopReason::GuestFault);
            GuestFaultInfo fault{};
            fault.guest_rip = rip;
            fault.access = GuestAccessKind::Execute;
            fault.recoverable = false;
            result.fault = fault;
        }

        result.snapshot =
            CaptureSnapshot(handle, entry,
                            result.primary_reason == StopReason::Returned ? SnapshotKind::SafePoint
                                                                          : SnapshotKind::Faulted);
        return result;
    }

    CpuConfig config_{};
    GuestAddressSpace &space_;

    // Identity for tickets and receipts. Monotonic across the process, so a ticket from a
    // destroyed context cannot match its replacement even though thread ids restart.
    const std::uint64_t context_id_{NextContextId()};
    std::uint64_t next_request_epoch_{0};

    mutable std::mutex lock_;
    std::condition_variable stopped_changed_;
    std::unordered_map<std::uint64_t, ThreadEntry> threads_;
    std::mutex coordinator_lock_;
    std::optional<QuiescenceDrain> drain_;
    std::vector<InterruptTicket> drain_tickets_;
    std::uint64_t next_thread_id_{1};

    fextl::unique_ptr<FEXCore::Context::Context> context_;
    std::unique_ptr<FexSignalDelegator> signal_delegator_;
    std::unique_ptr<FexSyscallHandler> syscall_handler_;
#if defined(GUEST_CPU_TEST_HOOKS)
    // Constructed eagerly with the context (single-threaded, before any Run), so owner threads that
    // read this in Run never race the lazy creation that a lazily-assigned unique_ptr would allow.
    std::unique_ptr<FexTestRunGate> test_run_gate_{std::make_unique<FexTestRunGateImpl>()};
    std::unique_ptr<FexTestRunGate> test_continuation_gate_{std::make_unique<FexTestRunGateImpl>()};
    std::atomic<std::uint64_t> test_continuation_retries_{};
    // N3 probe syscall-point trace (test builds only); the immediate-exit wrapper itself is
    // production and always installed, not gated by this flag.
    bool test_syscall_trace_enabled_{false};
#endif
    FEXCore::HostFeatures host_features_{};
    ReturnGate return_gate_;
    std::uint64_t host_page_size_{};
};

} // namespace

BackendCapabilities QueryFexCapabilities() {
    BackendCapabilities caps{};
    caps.backend_name = "FEXCore";
    caps.upstream_revision = "50e6eee95ae95d3257672727a9302a30b4a60a9a";
    caps.downstream_revision = "feature/malos/host-page-size";
    // AVX is deliberately absent: C04 is a conditional MUST, and declaring a
    // feature without an execution and state-restore test would be a false
    // claim. C03 verifies that asking for it is refused.
    caps.features = GuestFeature::BaseInteger | GuestFeature::Sse2;
    // Step is refused before execution until the block-limit path is driven, so
    // no scope is claimed.
    caps.step_scope = StepScope::None;
    caps.host_page_size = FEXCore::Utils::HostPageSize();
    caps.max_guest_address = kGuestAddressPolicyLimit;
    return caps;
}

Result<std::unique_ptr<CpuContext>> CreateFexContext(const CpuConfig &config,
                                                     GuestAddressSpace &space) {
    if (config.api_version != CpuConfig::kApiVersion) {
        return BackendError(ErrorCategory::InvalidArgument, "CreateContext",
                            "api_version does not match this build");
    }
    if (config.memory_mode != MemoryMode::DirectMapped) {
        return BackendError(ErrorCategory::UnsupportedMemoryMode, "CreateContext",
                            "V0 implements DirectMapped only");
    }
    if (config.smc_mode != SmcMode::ExplicitPublication) {
        return BackendError(ErrorCategory::Unsupported, "CreateContext",
                            "V0 implements ExplicitPublication only");
    }
    if (Contains(config.requested_features, GuestFeature::Avx)) {
        return BackendError(ErrorCategory::Unsupported, "CreateContext",
                            "AVX is not implemented; capabilities do not declare it");
    }

    bool expected = false;
    if (!g_context_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return BackendError(ErrorCategory::AlreadyActive, "CreateContext",
                            "a guest CPU context is already live in this process");
    }

    auto context = std::make_unique<FexCpuContext>(config, space);
    auto init = context->Initialize();
    if (!init) {
        // Reset the flag here rather than only in the destructor: a failed
        // Initialize must not leave the process unable to try again.
        g_context_active.store(false, std::memory_order_release);
        return init.GetError();
    }
    return std::unique_ptr<CpuContext>{std::move(context)};
}

void* FexHleRegistryPointer(CpuContext& context) {
    // Only FexCpuContext is constructed in this TU.
    return static_cast<FexCpuContext*>(&context)->HleRegistryPointer();
}

#if defined(GUEST_CPU_TEST_HOOKS)
void* FexTestRunGatePointer(CpuContext& context) {
    return static_cast<FexCpuContext*>(&context)->TestRunGatePointer();
}

void* FexTestContinuationGatePointer(CpuContext& context) {
    return static_cast<FexCpuContext*>(&context)->TestContinuationGatePointer();
}

std::uint64_t FexTestContinuationRetries(CpuContext& context) {
    return static_cast<FexCpuContext*>(&context)->TestContinuationRetries();
}

// N3 probe diagnostic: enable syscall-point trace for the next Run and read the last record. The
// immediate-exit wrapper itself is production (no arm switch); this only turns on trace capture.
void FexTestSetSyscallTrace(CpuContext& context, bool trace) {
    static_cast<FexCpuContext*>(&context)->SetSyscallTraceEnabled(trace);
}
const FexSyscallTraceRecord* FexTestSyscallTrace() { return &g_syscall_trace_probe; }
#endif

} // namespace Core::GuestCpu::Fex

namespace Core::GuestCpu {

Result<std::unique_ptr<CpuContext>> CreateContext(const CpuConfig &config,
                                                  GuestAddressSpace &space) {
    return Fex::CreateFexContext(config, space);
}

BackendCapabilities QueryBackendCapabilities() { return Fex::QueryFexCapabilities(); }

} // namespace Core::GuestCpu
