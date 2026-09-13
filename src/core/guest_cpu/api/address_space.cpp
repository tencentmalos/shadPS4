// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/api/access_fault.h"
#include "core/guest_cpu/api/address_space.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

#include <sys/mman.h>
#include <unistd.h>

namespace Core::GuestCpu {
std::atomic<AccessFaultHandler*> AccessFaultHandler::active{};
std::atomic<unsigned> AccessFaultHandler::readers{};
namespace {

// Test seam for mmap/mprotect results. Production always calls the real syscall; the wrappers below
// default to invoking it. Test builds can fail before invoking the syscall, so the error
// identity and fail-closed behaviour can be exercised deterministically
// without relying on resource exhaustion. The hooks are not reachable from the public API headers.
#if defined(GUEST_CPU_TEST_HOOKS)
using MmapFn = void* (*)(void*, size_t, int, int, int, off_t);
using ProtectFn = int (*)(void*, size_t, int);
std::atomic<MmapFn> g_mmap_hook{nullptr};
std::atomic<ProtectFn> g_protect_hook{nullptr};

void* SeamMmap(void* addr, size_t length, int prot, int flags, int fd, off_t off) {
    auto hook = g_mmap_hook.load(std::memory_order_acquire);
    return hook ? hook(addr, length, prot, flags, fd, off)
                : ::mmap(addr, length, prot, flags, fd, off);
}
int SeamMprotect(void* addr, size_t length, int prot) {
    auto hook = g_protect_hook.load(std::memory_order_acquire);
    return hook ? hook(addr, length, prot) : ::mprotect(addr, length, prot);
}

} // namespace

// Internal test controls. Not declared in any public header; declared here for the contract test
// that links this translation unit. We are inside namespace Core::GuestCpu, so a relative Test
// namespace resolves to Core::GuestCpu::Test with external linkage.
namespace Test {
void SetMmapFailureForTest(bool enable) {
    g_mmap_hook.store(enable ? [](void*, size_t, int, int, int, off_t) -> void* {
                          errno = ENOMEM;
                          return MAP_FAILED;
                      }
                      : nullptr,
                  std::memory_order_release);
}
void SetMprotectFailureForTest(bool enable) {
    g_protect_hook.store(enable ? [](void*, size_t, int) -> int {
                            errno = EACCES;
                            return -1;
                        }
                        : nullptr,
                        std::memory_order_release);
}
} // namespace Test

#else
void* SeamMmap(void* addr, size_t length, int prot, int flags, int fd, off_t off) {
    return ::mmap(addr, length, prot, flags, fd, off);
}
int SeamMprotect(void* addr, size_t length, int prot) {
    return ::mprotect(addr, length, prot);
}
} // namespace
#endif

namespace {

std::uint64_t DiscoverHostPageSize() {
    const long value = ::sysconf(_SC_PAGESIZE);
    // A non-positive or non-power-of-two answer means we cannot reason about
    // alignment at all. Falling back to a guess would be worse than failing
    // loudly later, but we must return something; use the smallest sane value
    // so that alignment checks stay conservative.
    if (value <= 0) {
        return 4096;
    }
    const auto page = static_cast<std::uint64_t>(value);
    if ((page & (page - 1)) != 0) {
        return 4096;
    }
    return page;
}

int ToHostProtection(GuestPermission permission) {
    int prot = PROT_NONE;
    if (HasPermission(permission, GuestPermission::Read)) {
        prot |= PROT_READ;
    }
    if (HasPermission(permission, GuestPermission::Write)) {
        prot |= PROT_WRITE;
    }
    if (HasPermission(permission, GuestPermission::Execute)) {
        prot |= PROT_EXEC;
    }
    return prot;
}

bool RangesOverlap(const GuestRange& a, const GuestRange& b) {
    return a.base.value < b.base.value + b.size && b.base.value < a.base.value + a.size;
}

// mmap failures are not all "out of memory". A W^X host refuses RWX with
// EACCES, and reporting that as OutOfMemory would send a reader looking for a
// memory leak instead of a permission policy.
ErrorCategory CategoriseMapFailure(int error) {
    switch (error) {
    case EACCES:
    case EPERM:
        return ErrorCategory::PermissionDenied;
    case EINVAL:
        return ErrorCategory::InvalidArgument;
    case ENOMEM:
    case EAGAIN:
        return ErrorCategory::OutOfMemory;
    default:
        return ErrorCategory::BackendFailure;
    }
}

} // namespace

std::uint64_t HostPageSize() {
    static const std::uint64_t cached = DiscoverHostPageSize();
    return cached;
}

std::uint64_t AlignUpToHostPage(std::uint64_t value) {
    const std::uint64_t page = HostPageSize();
    if (value > std::numeric_limits<std::uint64_t>::max() - (page - 1)) {
        return 0;  // Overflow: report failure rather than wrapping to a small value.
    }
    return (value + page - 1) & ~(page - 1);
}

std::string_view ToString(SmcMode mode) noexcept {
    switch (mode) {
    case SmcMode::ExplicitPublication: return "ExplicitPublication";
    case SmcMode::TransparentSmc: return "TransparentSMC";
    }
    return "Unknown";
}

Result<GuestRange> GuestRange::Checked(GuestAddress base, std::uint64_t size) {
    if (size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestRange::Checked",
                         "zero-length range");
    }
    if (base.value > std::numeric_limits<std::uint64_t>::max() - size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestRange::Checked",
                         "base + size overflows");
    }
    return GuestRange{base, size};
}

// --- QuiescenceToken --------------------------------------------------------

QuiescenceToken::QuiescenceToken(std::weak_ptr<AddressSpaceLiveness> owner_,
                                 std::uint64_t epoch_, std::size_t threads)
    : owner{std::move(owner_)}, epoch{epoch_}, stopped_threads{threads} {}

QuiescenceToken::QuiescenceToken(QuiescenceToken&& other) noexcept
    : owner{std::move(other.owner)}, epoch{other.epoch},
      stopped_threads{other.stopped_threads} {
    other.owner.reset();
    other.epoch = 0;
    other.stopped_threads = 0;
}

QuiescenceToken& QuiescenceToken::operator=(QuiescenceToken&& other) noexcept {
    if (this != &other) {
        ReleaseIfOwned();
        owner = std::move(other.owner);
        epoch = other.epoch;
        stopped_threads = other.stopped_threads;
        other.owner.reset();
        other.epoch = 0;
        other.stopped_threads = 0;
    }
    return *this;
}

QuiescenceToken::~QuiescenceToken() {
    ReleaseIfOwned();
}

void QuiescenceToken::ReleaseIfOwned() noexcept {
    if (epoch == 0) {
        return;
    }
    // lock() fails when the address space is already gone. Releasing into a
    // destroyed space is then correctly a no-op rather than a call through a
    // dangling pointer.
    if (auto alive = owner.lock(); alive && alive->space != nullptr) {
        alive->space->ReleaseQuiescence(epoch);
    }
    owner.reset();
    epoch = 0;
}

// --- ExecutionLease ---------------------------------------------------------
//
// Held by the backend for the duration of one Run or Step. Unlike QuiescenceToken this stores a
// raw pointer rather than a weak reference: the backend cannot outlive the address space it was
// created against (CreateContext documents that `space` must outlive the context), and a lease
// never escapes a single Run call.

ExecutionLease::ExecutionLease(ExecutionLease&& other) noexcept : space{other.space} {
    other.space = nullptr;
}

ExecutionLease& ExecutionLease::operator=(ExecutionLease&& other) noexcept {
    if (this != &other) {
        ReleaseIfOwned();
        space = other.space;
        other.space = nullptr;
    }
    return *this;
}

ExecutionLease::~ExecutionLease() {
    ReleaseIfOwned();
}

void ExecutionLease::ReleaseIfOwned() noexcept {
    if (space == nullptr) {
        return;
    }
    space->ReleaseExecutionLease();
    space = nullptr;
}

// --- PinnedSpan -------------------------------------------------------------

PinnedSpan::PinnedSpan(std::weak_ptr<AddressSpaceLiveness> owner_, GuestAddress base,
                       std::byte* data, std::size_t size, bool writable_, std::uint64_t lease)
    : owner{std::move(owner_)}, guest_base{base}, host_data{data}, host_size{size},
      writable{writable_}, lease_id{lease} {}

PinnedSpan::PinnedSpan(PinnedSpan&& other) noexcept
    : owner{std::move(other.owner)}, guest_base{other.guest_base}, host_data{other.host_data},
      host_size{other.host_size}, writable{other.writable}, lease_id{other.lease_id} {
    other.owner.reset();
    other.host_data = nullptr;
    other.host_size = 0;
    other.lease_id = 0;
}

PinnedSpan& PinnedSpan::operator=(PinnedSpan&& other) noexcept {
    if (this != &other) {
        Release();
        owner = std::move(other.owner);
        guest_base = other.guest_base;
        host_data = other.host_data;
        host_size = other.host_size;
        writable = other.writable;
        lease_id = other.lease_id;
        other.owner.reset();
        other.host_data = nullptr;
        other.host_size = 0;
        other.lease_id = 0;
    }
    return *this;
}

PinnedSpan::~PinnedSpan() {
    Release();
}

void PinnedSpan::Release() {
    if (lease_id != 0) {
        // Same rule as QuiescenceToken: a lease that outlived its space is
        // dropped silently instead of writing through freed memory.
        if (auto alive = owner.lock(); alive && alive->space != nullptr) {
            alive->space->ReleasePin(lease_id);
        }
    }
    owner.reset();
    host_data = nullptr;
    host_size = 0;
    lease_id = 0;
}

// --- GuestAddressSpace ------------------------------------------------------

GuestAddressSpace::GuestAddressSpace(void* reservation, std::uint64_t size,
                                     const AddressSpaceConfig& config)
    : reservation_host{reservation},
      reservation_base{GuestAddress{reinterpret_cast<std::uint64_t>(reservation)}},
      reservation_size{size}, memory_mode{config.memory_mode}, smc_mode{config.smc_mode},
      liveness{std::make_shared<AddressSpaceLiveness>()} {
    reservations.push_back({reservation_base, reservation_size});
    liveness->space = this;
}

Result<std::unique_ptr<GuestAddressSpace>> GuestAddressSpace::Create(
    const AddressSpaceConfig& config) {
    // Refuse unimplemented modes up front. Downgrading silently would let a
    // caller believe it had MMIO or transparent SMC support (DEC-05).
    if (config.memory_mode != MemoryMode::DirectMapped) {
        return MakeError(ErrorCategory::UnsupportedMemoryMode, "GuestAddressSpace::Create",
                         "V0 implements DirectMapped only");
    }
    if (config.smc_mode != SmcMode::ExplicitPublication) {
        return MakeError(ErrorCategory::Unsupported, "GuestAddressSpace::Create",
                         "V0 implements ExplicitPublication SMC only; see DEC-05");
    }

    const std::uint64_t size = AlignUpToHostPage(config.reservation_size);
    if (size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                         "reservation size is zero or overflows when aligned");
    }

    if (!config.owned_ranges.empty()) {
        const auto envelope = GuestRange::Checked(GuestAddress{config.preferred_base}, size);
        if (!envelope || !config.preferred_base ||
            (config.max_address && envelope.Value().End() > config.max_address))
            return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                             "invalid segmented envelope");
        std::uint64_t previous_end = config.preferred_base;
        for (auto range : config.owned_ranges) {
            const auto checked = GuestRange::Checked(range.base, range.size);
            if (!checked || !IsHostPageAligned(range.base.value) ||
                !IsHostPageAligned(range.size) || range.base.value < previous_end ||
                checked.Value().End() > envelope.Value().End())
                return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                                 "invalid/disordered owned ranges");
            previous_end = checked.Value().End();
        }
        auto owned = config.owned_ranges;
        std::size_t held = 0;
        auto release = [&] {
            for (std::size_t i = 0; i < held; ++i)
                ::munmap(reinterpret_cast<void*>(config.owned_ranges[i].base.value),
                         config.owned_ranges[i].size);
        };
        for (auto range : config.owned_ranges) {
            void* mapped = ::mmap(reinterpret_cast<void*>(range.base.value), range.size, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (mapped == MAP_FAILED ||
                reinterpret_cast<std::uint64_t>(mapped) != range.base.value) {
                if (mapped != MAP_FAILED)
                    ::munmap(mapped, range.size);
                release();
                return MakeError(ErrorCategory::OutOfMemory, "GuestAddressSpace::Create",
                                 "exact guest segment is occupied; host mappings preserved");
            }
            ++held;
        }
        try {
            auto result = std::unique_ptr<GuestAddressSpace>(new GuestAddressSpace(
                reinterpret_cast<void*>(config.preferred_base), size, config));
            // Replace only the ownership record; never unmap the envelope.
            result->reservations.swap(owned);
            return result;
        } catch (...) {
            release();
            throw;
        }
    }

    // PROT_NONE reservation. The kernel picks the address: we never scan
    // /proc/self/maps for a hole and then MAP_FIXED into it, because ART or a
    // driver can claim that hole in between (spec §5.1).
    //
    // When the caller caps the address, pass a hint and then check what we got.
    // A hint is advisory -- the kernel may ignore it -- so the check below is
    // what actually enforces the limit; the hint just makes success likely.
    void* hint = nullptr;
    if (config.max_address != 0) {
        if (size > config.max_address) {
            return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                             "reservation is larger than the requested maximum address");
        }
        // Aim well below the cap so the mapping has room to fall back downward.
        hint = reinterpret_cast<void*>((config.max_address - size) / 2);
    }

    if (config.preferred_base) {
        auto checked = GuestRange::Checked(GuestAddress{config.preferred_base}, size);
        if (!checked || !IsHostPageAligned(config.preferred_base) ||
            (config.max_address && checked.Value().End() > config.max_address))
            return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                             "invalid requested placement");
        hint = reinterpret_cast<void*>(config.preferred_base);
    }
    void* reservation = ::mmap(hint, static_cast<std::size_t>(size), PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED) {
        const int saved = errno;
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Create",
                               "reservation mmap failed");
        error.system_error = saved;
        return error;
    }

    if (config.preferred_base &&
        reinterpret_cast<std::uint64_t>(reservation) != config.preferred_base) {
        ::munmap(reservation, static_cast<std::size_t>(size));
        return MakeError(ErrorCategory::OutOfMemory, "GuestAddressSpace::Create",
                         "requested guest placement is occupied; no host mapping was replaced");
    }

    if (config.max_address &&
        reinterpret_cast<std::uint64_t>(reservation) > config.max_address - size) {
        ::munmap(reservation, static_cast<std::size_t>(size));
        reservation = MAP_FAILED;
        // The kernel can ignore a colliding hint even while lower space exists.
        // Try bounded, separated hints; each mmap claims its range atomically.
        // Never infer ownership from /proc/maps or overwrite another mapping.
        for (unsigned slot = 1; slot < 32; ++slot) {
            const auto candidate =
                ((config.max_address - size) / 32 * slot) & ~(HostPageSize() - 1);
            if (candidate < HostPageSize())
                continue;
            void* mapped = ::mmap(reinterpret_cast<void*>(candidate), size, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (mapped == MAP_FAILED)
                continue;
            if (reinterpret_cast<std::uint64_t>(mapped) <= config.max_address - size) {
                reservation = mapped;
                break;
            }
            ::munmap(mapped, size);
        }
        if (reservation == MAP_FAILED)
            return MakeError(ErrorCategory::OutOfMemory, "GuestAddressSpace::Create",
                             "no bounded reservation attempt landed below the requested maximum");
    }

    return std::unique_ptr<GuestAddressSpace>(
        new GuestAddressSpace(reservation, size, config));
}

GuestAddressSpace::~GuestAddressSpace() {
    // Publish "gone" before releasing memory, so any surviving token or span
    // sees an expired owner rather than a stale pointer.
    if (liveness) {
        liveness->space = nullptr;
    }
    for (auto range : reservations)
        ::munmap(reinterpret_cast<void*>(range.base.value), static_cast<std::size_t>(range.size));
}

bool GuestAddressSpace::OwnsRange(GuestRange range) const {
    const auto checked = GuestRange::Checked(range.base, range.size);
    if (!checked)
        return false;
    for (auto owned : reservations)
        if (range.base.value >= owned.base.value && checked.Value().End() <= owned.End())
            return true;
    return false;
}

std::uint64_t GuestAddressSpace::MappingGeneration() const {
    std::lock_guard guard{lock};
    return mapping_generation;
}

std::uint64_t GuestAddressSpace::CodeGeneration() const {
    std::lock_guard guard{lock};
    return code_generation;
}

std::byte* GuestAddressSpace::HostPointer(GuestAddress address) const {
    return reinterpret_cast<std::byte*>(address.value);
}

Result<MappingInfo> GuestAddressSpace::Map(GuestRange range, GuestPermission permission) {
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "zero-length range");
    }
    if (range.base.value > std::numeric_limits<std::uint64_t>::max() - range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "base + size overflows");
    }
    // Host-page alignment, not guest-page alignment. Accepting a 4 KiB-aligned
    // request on a 16 KiB host and widening it silently would grant
    // permissions on memory the caller never asked about (spec §5.1).
    if (!IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size)) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "range must be host-page aligned; host page is " +
                             std::to_string(HostPageSize()));
    }
    if (!OwnsRange(range)) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "range is outside this address space's reservation");
    }

    std::lock_guard guard{lock};
    if (auto status = CheckMappingMutationLocked("GuestAddressSpace::Map"); !status) {
        return status.GetError();
    }
    for (const auto& existing : mappings) {
        if (RangesOverlap(existing.range, range)) {
            return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                             "range overlaps an existing mapping");
        }
    }

    // MAP_FIXED is safe here and only here: the target is inside a reservation
    // this object already owns, so nothing else can be holding it.
    void* result = SeamMmap(HostPointer(range.base), static_cast<std::size_t>(range.size),
                            ToHostProtection(permission),
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        const int saved = errno;
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Map",
                               "commit mmap failed");
        error.system_error = saved;
        return error;
    }

    ++mapping_generation;
    Mapping mapping{range, permission, ProtectionReason::GuestPermission, mapping_generation};
    mappings.push_back(mapping);

    MappingInfo info{};
    info.range = range;
    info.permission = permission;
    info.reasons = mapping.reasons;
    info.mapping_generation = mapping.generation;
    info.host_owned = true;
    return info;
}

Status GuestAddressSpace::RetireBeforeMutationLocked(std::unique_lock<std::mutex>& guard,
                                                    GuestRange range) {
    bool committed = false;
    auto status = CallSinkUnlocked(guard, range, InvalidationReason::Unmap, committed);
    ++code_generation;
    if (!committed) {
        PoisonCodeLocked(range);
        return status;
    }
    return Ok();
}

Status GuestAddressSpace::Unmap(GuestRange range) {
    std::unique_lock guard{lock};
    if (auto status = CheckMappingMutationLocked("GuestAddressSpace::Unmap"); !status) {
        return status;
    }

    // A live HLE lease wins: the span must stay valid for its whole lifetime
    // (acceptance M13). Report Busy rather than pulling it out underneath.
    for (const auto& pin : pins) {
        if (RangesOverlap(pin.range, range)) {
            return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Unmap",
                             "range has a live pinned HLE span");
        }
    }

    const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base == range.base && m.range.size == range.size;
    });
    if (found == mappings.end()) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Unmap",
                         "no mapping exactly matches this range");
    }

    // Guest page permissions do not revoke FEX's separate host JIT mappings. Retire
    // shared/local/decoder entries before changing permissions/backing, even for RW -> RX.
    if (auto retired = RetireBeforeMutationLocked(guard, range); !retired) return retired;
    // Return the range to PROT_NONE while keeping the reservation: dropping it
    // with munmap would let an unrelated allocation land inside our space.
    void* result = SeamMmap(HostPointer(range.base), static_cast<std::size_t>(range.size),
                          PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        const int saved = errno;
        PoisonCodeLocked(range);
        auto error = MakeError(ErrorCategory::BackendFailure, "GuestAddressSpace::Unmap",
                               "failed to restore reservation protection");
        error.system_error = saved;
        return error;
    }

    mappings.erase(found);
    std::erase_if(aliases, [&](const Alias& alias) {
        return RangesOverlap(alias.primary, range);
    });
    ++mapping_generation;
    ++code_generation;
    return Ok();
}

Status GuestAddressSpace::Protect(GuestRange range, GuestPermission permission) {
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Protect",
                         "zero-length range");
    }

    // The M03 case. On a 16 KiB host a 4 KiB sub-range cannot get its own
    // protection, so we refuse *before* touching anything: permissions,
    // contents and bookkeeping all stay exactly as they were (DEC-06).
    if (!IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size)) {
        return MakeError(
            ErrorCategory::Unsupported, "GuestAddressSpace::Protect",
            "DirectMapped cannot give sub-host-page ranges distinct permissions; host page is " +
                std::to_string(HostPageSize()) + ", nothing was modified");
    }

    std::unique_lock guard{lock};
    const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base.value <= range.base.value &&
               range.base.value + range.size <= m.range.base.value + m.range.size;
    });
    if (found == mappings.end()) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Protect",
                         "range is not inside a single mapping");
    }

    // Reject a partial range instead of recording the new permission for the whole mapping.
    //
    // The 2026-09-08 review (R3) reproduced the bug this replaces: mprotect was applied to the
    // sub-range while `found->permission` was overwritten for the entire mapping, so ValidateRange
    // then approved writes to pages the kernel still had read-only, and a handed-out writable span
    // faulted on first use. Splitting mappings would be the general answer; refusing is the
    // honest one until that exists, and it never leaves the table disagreeing with the kernel.
    if (range.base.value != found->range.base.value || range.size != found->range.size) {
        return MakeError(ErrorCategory::Unsupported, "GuestAddressSpace::Protect",
                         "this implementation cannot split a mapping; Protect must cover the whole "
                         "mapping, and nothing was modified");
    }

    // A live pin means someone holds a host pointer obtained under the current permission.
    // Changing it underneath them would invalidate a span they are still allowed to use, which
    // M13 forbids; the review recorded the missing check as R4.
    if (AnyPinOverlapsLocked(range)) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Protect",
                         "a pinned span overlaps this range; release it before reprotecting");
    }

    // Re-granting execute over a poisoned range would undo the only protection a failed
    // publication has: the bytes changed, the backend still holds a translation of the old ones,
    // and revoking execute here is what keeps that translation unreachable. The review made a
    // failed publication executable again with exactly this call, and the guest re-ran the stale
    // constant (R1).
    if (HasPermission(permission, GuestPermission::Execute) &&
        std::any_of(poisoned_ranges.begin(), poisoned_ranges.end(),
                    [&](GuestRange poisoned) { return RangesOverlap(range, poisoned); })) {
        return MakeError(ErrorCategory::WrongState, "GuestAddressSpace::Protect",
                         "code publication failed over this range and has not been repaired; "
                         "execute cannot be granted");
    }

    if (auto status = CheckMappingMutationLocked("GuestAddressSpace::Protect"); !status) {
        return status;
    }
    // Guest page permissions do not revoke FEX's separate host JIT mappings. Retire
    // shared/local/decoder entries before changing permissions/backing, even for RW -> RX.
    if (auto retired = RetireBeforeMutationLocked(guard, range); !retired) return retired;
    if (SeamMprotect(HostPointer(range.base), static_cast<std::size_t>(range.size),
                      ToHostProtection(permission)) != 0) {
        const int saved = errno;
        PoisonCodeLocked(range);
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Protect",
                               "mprotect failed");
        error.system_error = saved;
        return error;
    }

    found->permission = permission;
    found->generation = ++mapping_generation;
    return Ok();
}

Result<MappingInfo> GuestAddressSpace::Query(GuestAddress address) const {
    std::lock_guard guard{lock};
    for (const auto& mapping : mappings) {
        if (address.value >= mapping.range.base.value &&
            address.value < mapping.range.base.value + mapping.range.size) {
            MappingInfo info{};
            info.range = mapping.range;
            info.permission = mapping.permission;
            info.reasons = mapping.reasons;
            info.mapping_generation = mapping.generation;
            info.host_owned = true;
            return info;
        }
    }
    return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Query",
                     "address is not mapped");
}

std::vector<MappingInfo> GuestAddressSpace::Mappings() const {
    std::lock_guard guard{lock};
    std::vector<MappingInfo> out;
    out.reserve(mappings.size());
    for (const auto& mapping : mappings) {
        MappingInfo info{};
        info.range = mapping.range;
        info.permission = mapping.permission;
        info.reasons = mapping.reasons;
        info.mapping_generation = mapping.generation;
        info.host_owned = true;
        out.push_back(info);
    }
    return out;
}

Status GuestAddressSpace::ValidateRange(GuestRange range, GuestPermission required) const {
    std::lock_guard guard{lock};
    return ValidateRangeLocked(range, required);
}

// Requires lock. Separated from ValidateRange so a caller that must validate and then act
// atomically -- AcquirePinnedSpan, Read, Write -- can hold one lock across both instead of
// revalidating against state that may already have changed.
Status GuestAddressSpace::ValidateRangeLocked(GuestRange range, GuestPermission required) const {
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                         "zero-length range");
    }
    if (range.base.value > std::numeric_limits<std::uint64_t>::max() - range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                         "base + size overflows");
    }

    for (const auto& mapping : mappings) {
        if (range.base.value >= mapping.range.base.value &&
            range.base.value + range.size <= mapping.range.base.value + mapping.range.size) {
            if (!HasPermission(mapping.permission, required)) {
                return MakeError(ErrorCategory::PermissionDenied,
                                 "GuestAddressSpace::ValidateRange",
                                 "mapping lacks the requested permission");
            }
            return Ok();
        }
    }
    // Deliberately not merging adjacent mappings: a span crossing two separate
    // mappings is rejected, because their permissions can differ.
    return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                     "range is not contained in a single mapping");
}

Status GuestAddressSpace::Read(GuestAddress from, std::span<std::byte> into) const {
    const auto range = GuestRange::Checked(from, into.size());
    if (!range) {
        return range.GetError();
    }
    // Copy under the same lock that validated the range: releasing it first would let an Unmap or
    // Protect land between the check and the memcpy (2026-09-08 review, R4).
    std::lock_guard guard{lock};
    if (auto status = ValidateRangeLocked(range.Value(), GuestPermission::Read); !status) {
        return status;
    }
    std::memcpy(into.data(), HostPointer(from), into.size());
    return Ok();
}

Status GuestAddressSpace::Write(GuestAddress to, std::span<const std::byte> from) {
    const auto range = GuestRange::Checked(to, from.size());
    if (!range) {
        return range.GetError();
    }
    {
        std::lock_guard guard{lock};

        // Same admission rule as AcquirePinnedSpan. Without it a transaction holding quiescence
        // could still be undercut through the explicit write path, which would make the token mean
        // "no pinned writers" rather than "no writers" -- and those are only the same if every
        // writer happens to go through a pin.
        //
        // The transaction owner is not blocked by this: it publishes through PublishCode, which
        // has its own epoch check and does not route here.
        if (active_quiescence != 0 || sink_calls_in_flight != 0 || sink_draining) {
            return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Write",
                             "a transaction or sink callback is active; ordinary writes are excluded");
        }

        if (auto status = ValidateRangeLocked(range.Value(), GuestPermission::Write); !status) {
            return status;
        }
        std::memcpy(HostPointer(to), from.data(), from.size());
    }
    // Observers are notified with the lock released: a callback may re-enter the address space,
    // and holding the lock across it would deadlock.
    NotifyObservers(range.Value());
    return Ok();
}

Result<PinnedSpan> GuestAddressSpace::AcquirePinnedSpan(GuestRange range, bool writable) {
    const auto required = writable ? (GuestPermission::Read | GuestPermission::Write)
                                   : GuestPermission::Read;

    // Validation and lease registration happen under one lock hold. Splitting them -- validate,
    // unlock, relock, register -- let an Unmap or Protect land in between and hand back a span for
    // a range that had already changed. The review recorded that window as part of R4.
    std::lock_guard guard{lock};

    // Remap releases this lock while retiring old translations. Even a read pin must not enter
    // that interval: it would retain a pointer to the backing that MAP_FIXED is about to replace.
    if (sink_calls_in_flight != 0 || sink_draining)
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::AcquirePinnedSpan",
                         "publication or remap is in flight or draining");

    // A transaction holds quiescence precisely so it can change code or mappings without a writer
    // underneath it. Admitting a new writable pin during one would reopen the window the token is
    // supposed to have closed (2026-09-08 review, R5). Readers are still allowed: the transaction
    // owner needs them, and they cannot invalidate its assumptions.
    if (writable && active_quiescence != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::AcquirePinnedSpan",
                         "a quiescence transaction is in progress; no new writable span is granted")
            ;
    }

    if (auto status = ValidateRangeLocked(range, required); !status) {
        return status.GetError();
    }

    const std::uint64_t lease = next_lease_id++;
    pins.push_back(Pin{lease, range, writable});
    return PinnedSpan{liveness, range.base, HostPointer(range.base),
                      static_cast<std::size_t>(range.size), writable, lease};
}

// Requires lock. True when any live lease overlaps `range`.
bool GuestAddressSpace::AnyPinOverlapsLocked(GuestRange range) const {
    return std::any_of(pins.begin(), pins.end(),
                       [&](const Pin& pin) { return RangesOverlap(pin.range, range); });
}

void GuestAddressSpace::ReleasePin(std::uint64_t lease_id) {
    std::lock_guard guard{lock};
    std::erase_if(pins, [&](const Pin& pin) { return pin.lease_id == lease_id; });
    leases_idle.notify_all();
}

Result<QuiescenceDrain> GuestAddressSpace::BeginDrain() {
    std::lock_guard guard{lock};
    if (active_quiescence || sink_draining || sink_calls_in_flight)
        return MakeError(ErrorCategory::Busy, "BeginDrain", "another transaction is active");
    active_quiescence = next_quiescence_epoch++;
    QuiescenceDrain drain;
    drain.reservation = QuiescenceToken{liveness, active_quiescence, 0};
    return drain;
}

Result<QuiescenceToken> GuestAddressSpace::FinishDrain(QuiescenceDrain& drain,
                                                       std::uint64_t timeout_ns,
                                                       std::size_t stopped_threads) {
    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(drain.reservation, "FinishDrain"); !status)
        return status.GetError();
    if (!leases_idle.wait_for(guard, std::chrono::nanoseconds(timeout_ns), [&] {
            return execution_leases == 0 && pins.empty();
        }))
        return MakeError(ErrorCategory::Timeout, "FinishDrain", "owners or HLE pins have not drained");
    if (sink_draining || sink_calls_in_flight)
        return MakeError(ErrorCategory::Busy, "FinishDrain", "sink is draining");
    drain.reservation.stopped_threads = stopped_threads;
    return std::move(drain.reservation);
}

Result<QuiescenceToken> GuestAddressSpace::Quiesce(std::uint64_t timeout_ns,
                                                   bool wait_for_leases) {
    std::unique_lock guard{lock};
    if (active_quiescence != 0 || sink_draining || sink_calls_in_flight != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Quiesce",
                         "another transaction already holds quiescence");
    }
    // Claim the epoch up front when asked to drain: this closes admission (AcquireExecutionLease
    // refuses while active_quiescence is set), then wait for leases already in flight to release.
    // The caller (the context coordinator) has already stopped every owner, so those leases are in
    // Run's tail and WILL drain; waiting bounded by the timeout cannot deadlock on dead code.
    //
    // Without wait_for_leases (a memory-only transaction with no one stopping running code) an
    // outstanding lease means genuinely running guest code, so report Busy and change nothing.
    const std::uint64_t epoch = next_quiescence_epoch++;
    active_quiescence = epoch;

    if (execution_leases != 0 && !wait_for_leases) {
        active_quiescence = 0;
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Quiesce",
                         "guest code is executing: " + std::to_string(execution_leases) +
                             " execution lease(s) outstanding");
    }
    if (wait_for_leases) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
        leases_idle.wait_for(guard, std::chrono::nanoseconds(timeout_ns),
                             [&] { return execution_leases == 0; });
        if (execution_leases != 0) {
            active_quiescence = 0;
            (void)deadline;
            return MakeError(ErrorCategory::Timeout, "GuestAddressSpace::Quiesce",
                             "guest code did not drain: " + std::to_string(execution_leases) +
                                 " execution lease(s) still outstanding");
        }
    }
    // In-flight HLE writers hold pins. Reporting Timeout without changing
    // anything is required: a partial stop must not be used to authorise a
    // transaction (API contract §7.2).
    if (!pins.empty()) {
        active_quiescence = 0;
        auto error = MakeError(ErrorCategory::Timeout, "GuestAddressSpace::Quiesce",
                               "HLE spans still pinned: " + std::to_string(pins.size()));
        error.system_error = static_cast<std::int64_t>(timeout_ns);
        return error;
    }

    // Thread stopping is the CpuContext's responsibility; it supplies the
    // count. With no attached context this is a memory-only transaction.
    return QuiescenceToken{liveness, epoch, 0};
}

void GuestAddressSpace::ReleaseQuiescence(std::uint64_t epoch) {
    std::lock_guard guard{lock};
    if (active_quiescence == epoch) {
        active_quiescence = 0;
        leases_idle.notify_all();
    }
}

Status GuestAddressSpace::WaitForQuiescenceRelease(std::uint64_t timeout_ns) {
    std::unique_lock guard{lock};
    if (!leases_idle.wait_for(guard, std::chrono::nanoseconds(timeout_ns),
                              [&] { return active_quiescence == 0; }))
        return MakeError(ErrorCategory::Timeout, "WaitForQuiescenceRelease",
                         "coordinator still owns the reservation");
    return Ok();
}

// Requires lock. Checks that a token authorises a transaction on *this* space right now.
//
// Epoch alone is not enough: every space counts epochs independently, so A's first token and B's
// first token are both epoch 1, and B would accept A's token while B's own transaction was live.
// The 2026-09-08 publication review reproduced exactly that (P1-B). Provenance is checked through
// the liveness block the token already carries, which also rejects a token whose space is gone.
Status GuestAddressSpace::CheckTokenLocked(const QuiescenceToken& token,
                                           std::string_view operation) const {
    if (!token.IsValid()) {
        return MakeError(ErrorCategory::InvalidArgument, operation,
                         "an invalid quiescence token cannot authorise this operation");
    }
    if (!token.IsFrom(this)) {
        return MakeError(ErrorCategory::InvalidArgument, operation,
                         "the token belongs to a different address space, or its space has been "
                         "destroyed");
    }
    if (token.Epoch() != active_quiescence) {
        return MakeError(ErrorCategory::StaleEpoch, operation,
                         "token epoch does not match the active transaction");
    }
    return Ok();
}

Status GuestAddressSpace::SetCodeInvalidationSink(CodeInvalidationSink* sink) {
    if (sink == nullptr) {
        return MakeError(ErrorCategory::InvalidArgument,
                         "GuestAddressSpace::SetCodeInvalidationSink",
                         "sink must not be null; use ClearCodeInvalidationSink");
    }
    std::lock_guard guard{lock};
    if (sink_draining) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::SetCodeInvalidationSink",
                         "the previous registration is still draining");
    }
    if (code_sink != nullptr && code_sink != sink) {
        // Replacing it would strand the previous backend's translations: nothing
        // would ever invalidate them again.
        return MakeError(ErrorCategory::AlreadyActive,
                         "GuestAddressSpace::SetCodeInvalidationSink",
                         "another backend is already registered with this address space");
    }
    if (code_sink != sink) {
        ++sink_generation;
    }
    code_sink = sink;
    return Ok();
}

void GuestAddressSpace::ClearCodeInvalidationSink(CodeInvalidationSink* sink) {
    std::unique_lock guard{lock};
    sink_idle.wait(guard, [&] { return !sink_draining; });
    if (code_sink != sink) {
        return;
    }
    code_sink = nullptr;
    ++sink_generation;
    sink_draining = true;
    sink_idle.wait(guard, [&] { return sink_calls_in_flight == 0; });
    sink_draining = false;
    sink_idle.notify_all();
}

bool GuestAddressSpace::HasPoisonedCode() const {
    std::lock_guard guard{lock};
    return !poisoned_ranges.empty();
}

bool GuestAddressSpace::IsQuiescent() const {
    std::lock_guard guard{lock};
    return active_quiescence != 0;
}

Result<ExecutionLease> GuestAddressSpace::AcquireExecutionLease() {
    std::lock_guard guard{lock};
    if (!poisoned_ranges.empty()) {
        // A failed publication left bytes that no longer match the backend's translation of them.
        // Executing anything in this space would risk running that translation.
        return MakeError(ErrorCategory::WrongState, "GuestAddressSpace::AcquireExecutionLease",
                         "code publication failed and has not been repaired; execution is blocked");
    }
    if (active_quiescence != 0 || sink_draining || sink_calls_in_flight != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::AcquireExecutionLease",
                         "a publication transaction holds this address space");
    }
    ++execution_leases;
    return ExecutionLease{this};
}

void GuestAddressSpace::ReleaseExecutionLease() {
    {
        std::lock_guard guard{lock};
        if (execution_leases > 0) {
            --execution_leases;
        }
        if (execution_leases != 0) {
            return;
        }
    }
    leases_idle.notify_all();
}

// Requires lock. Used when a publication cannot complete its backend half.
//
// Whole mappings only, matching Protect's refusal to split. A partial overlap therefore revokes
// execute from the entire containing mapping: over-revoking is safe here, leaving the range
// executable is not.
void GuestAddressSpace::RevokeExecuteLocked(GuestRange range) {
    for (auto& mapping : mappings) {
        if (!RangesOverlap(mapping.range, range)) {
            continue;
        }
        if (!HasPermission(mapping.permission, GuestPermission::Execute)) {
            continue;
        }
        const auto without_execute = static_cast<GuestPermission>(
            static_cast<std::uint32_t>(mapping.permission) &
            ~static_cast<std::uint32_t>(GuestPermission::Execute));
        if (::mprotect(HostPointer(mapping.range.base),
                       static_cast<std::size_t>(mapping.range.size),
                       ToHostProtection(without_execute)) == 0) {
            mapping.permission = without_execute;
            mapping.generation = ++mapping_generation;
        }
        // If even mprotect fails there is nothing further this layer can do; code_poisoned still
        // records that the range must not be trusted, and PublishCode returns the sink's error.
    }
}

// Calls the registered sink with the lock released, counting the call so ClearCodeInvalidationSink
// can wait for it. `guard` owns `lock` on entry and on return.
//
// `committed` is false when the registration changed while the callback ran: the sink that answered
// is no longer the one that owns translated code, so its answer cannot authorise anything.
Status GuestAddressSpace::CallSinkUnlocked(std::unique_lock<std::mutex>& guard, GuestRange range,
                                           InvalidationReason reason, bool& committed) {
    // Direct/file mappings may alias at different offsets. Until an indexed
    // backing graph is needed, explicit publication conservatively retires all
    // translations. A generation increment alone cannot retire an alias.
    if (shared_backing_seen) {
        range = {reservation_base, reservation_size};
        reason = InvalidationReason::FullFlush;
    }
    committed = false;
    CodeInvalidationSink* sink = code_sink;
    if (sink == nullptr) {
        // No backend attached: a memory-only transaction. The generation bump is the whole of it.
        committed = true;
        return Ok();
    }

    const std::uint64_t generation_at_entry = sink_generation;
    ++sink_calls_in_flight;
    guard.unlock();

    // Unlocked: the lock order is context -> space, because the backend reads CodeGeneration()
    // under its own lock to build a snapshot. Calling a sink from under `lock` would invert it.
    Status status;
    try {
        status = sink->DiscardTranslations(range, reason);
    } catch (...) {
        // No callback may leak the in-flight count or escape after bytes changed.
        status = MakeError(ErrorCategory::BackendFailure, "GuestAddressSpace::CallSinkUnlocked",
                           "backend invalidation threw an exception");
    }

    guard.lock();
    --sink_calls_in_flight;
    const bool same_registration = sink_generation == generation_at_entry;
    if (sink_calls_in_flight == 0) {
        // Notify with the lock held; Clear waits on this condition and re-checks the predicate.
        sink_idle.notify_all();
    }
    if (!same_registration) {
        return MakeError(ErrorCategory::WrongState, "GuestAddressSpace::PublishCode",
                         "the backend was unregistered while its invalidation was in flight");
    }
    committed = status.HasValue();
    return status;
}

Status GuestAddressSpace::InvalidateCode(const QuiescenceToken& token, GuestRange range,
                                         InvalidationReason reason) {
    auto checked = GuestRange::Checked(range.base, range.size);
    if (!checked) {
        return checked.GetError();
    }
    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(token, "GuestAddressSpace::InvalidateCode"); !status) {
        return status;
    }
    if (sink_draining || sink_calls_in_flight != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::InvalidateCode",
                         "another publication callback is in flight or draining");
    }

    bool committed = false;
    auto status = CallSinkUnlocked(guard, range, reason, committed);

    // Re-check: the token could have been released while the sink ran unlocked.
    if (auto token_status = CheckTokenLocked(token, "GuestAddressSpace::InvalidateCode");
        !token_status) {
        PoisonCodeLocked(range);
        ++code_generation;
        return token_status;
    }
    if (!committed) {
        // A caller may have changed bytes through a writable pin before invalidating.
        // Conservatively retain the execution block until a successful repair.
        PoisonCodeLocked(range);
        ++code_generation;
        return status ? MakeError(ErrorCategory::BackendFailure,
                                  "GuestAddressSpace::InvalidateCode",
                                  "invalidation did not commit")
                      : status;
    }

    // Bumping the generation is what makes "success" mean stale decode can no
    // longer be entered. Every executable alias of this backing is covered
    // because the generation is space-wide, not per-VA (acceptance M10).
    ++code_generation;
    // A successful invalidation over the poisoned range is a repair: the translations that no
    // longer matched the bytes are gone.
    ClearPoisonIfRepairedLocked(range);
    return Ok();
}

Status GuestAddressSpace::PublishCode(const QuiescenceToken& token, GuestRange range,
                                      std::span<const std::byte> code) {
    if (code.size() != range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::PublishCode",
                         "code length does not match the target range");
    }

    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(token, "GuestAddressSpace::PublishCode"); !status) {
        return status;
    }
    if (sink_draining || sink_calls_in_flight != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::PublishCode",
                         "another publication callback is in flight or draining");
    }

    // Validate and copy under one lock hold. Releasing it between the two would let a Protect or
    // Unmap land in the middle of a publication, which is precisely the state the token excludes.
    if (auto status = ValidateRangeLocked(range, GuestPermission::Write); !status) {
        return status;
    }
    std::memcpy(HostPointer(range.base), code.data(), code.size());

    bool committed = false;
    auto status = CallSinkUnlocked(guard, range, InvalidationReason::HostWrite, committed);

    if (!committed) {
        // The bytes are already new. From here the range must not become executable again: the
        // backend still holds a translation of the bytes that were there before, and running it
        // would execute code that no longer exists. Revoking execute on the guest pages is not
        // sufficient on its own -- the translation lives in the backend's own executable memory --
        // so poison additionally blocks execution leases and re-granting execute.
        PoisonCodeLocked(range);
        // Still advance: the bytes did change, so a generation captured before this call must not
        // continue to compare equal.
        ++code_generation;
        return status ? MakeError(ErrorCategory::BackendFailure, "GuestAddressSpace::PublishCode",
                                  "publication did not commit")
                      : status;
    }

    if (auto token_status = CheckTokenLocked(token, "GuestAddressSpace::PublishCode");
        !token_status) {
        PoisonCodeLocked(range);
        ++code_generation;
        return token_status;
    }
    ++code_generation;
    ClearPoisonIfRepairedLocked(range);
    return Ok();
}

Status GuestAddressSpace::ReprotectUnderToken(const QuiescenceToken& token, GuestRange range,
                                              GuestPermission permission) {
    if (auto checked = GuestRange::Checked(range.base, range.size); !checked)
        return checked.GetError();
    if (!IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size)) {
        return MakeError(ErrorCategory::Unsupported, "GuestAddressSpace::ReprotectUnderToken",
                         "range must be host-page aligned; nothing was modified");
    }
    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(token, "GuestAddressSpace::ReprotectUnderToken");
        !status) {
        return status;
    }
    if (sink_draining || sink_calls_in_flight != 0)
        return MakeError(ErrorCategory::Busy, "ReprotectUnderToken",
                         "a publication callback is in flight or draining");
    const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base.value == range.base.value && m.range.size == range.size;
    });
    if (found == mappings.end()) {
        return MakeError(ErrorCategory::InvalidArgument,
                         "GuestAddressSpace::ReprotectUnderToken",
                         "range is not an exact whole mapping; nothing was modified");
    }
    if (AnyPinOverlapsLocked(range)) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::ReprotectUnderToken",
                         "a pinned span overlaps this range");
    }
    // Poison still blocks execute: a failed publication must not become reachable by reprotecting
    // it inside a later, unrelated transaction.
    if (HasPermission(permission, GuestPermission::Execute) &&
        std::any_of(poisoned_ranges.begin(), poisoned_ranges.end(),
                    [&](GuestRange poisoned) { return RangesOverlap(range, poisoned); })) {
        return MakeError(ErrorCategory::WrongState,
                         "GuestAddressSpace::ReprotectUnderToken",
                         "code publication failed over this range and has not been repaired");
    }
    // Guest page permissions do not revoke FEX's separate host JIT mappings. Retire
    // shared/local/decoder entries before changing permissions/backing, even for RW -> RX.
    if (auto retired = RetireBeforeMutationLocked(guard, range); !retired) return retired;
    if (auto status = CheckTokenLocked(token, "ReprotectUnderToken"); !status) return status;
    if (SeamMprotect(HostPointer(range.base), static_cast<std::size_t>(range.size),
                      ToHostProtection(permission)) != 0) {
        const int saved = errno;
        PoisonCodeLocked(range);
        auto error = MakeError(CategoriseMapFailure(saved),
                               "GuestAddressSpace::ReprotectUnderToken",
                               "mprotect failed; requested protection was not committed; execution is poisoned");
        error.system_error = saved;
        return error;
    }
    found->permission = permission;
    found->generation = ++mapping_generation;
    return Ok();
}

Result<MappingInfo> GuestAddressSpace::RemapUnderToken(const QuiescenceToken& token,
                                                       GuestRange range,
                                                       GuestPermission permission) {
    if (auto checked = GuestRange::Checked(range.base, range.size); !checked)
        return checked.GetError();
    if (!IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size)) {
        return MakeError(ErrorCategory::InvalidArgument,
                         "GuestAddressSpace::RemapUnderToken",
                         "range must be host-page aligned");
    }
    if (!OwnsRange(range)) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::RemapUnderToken",
                         "range is outside this address space's reservation");
    }
    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(token, "GuestAddressSpace::RemapUnderToken"); !status) {
        return status.GetError();
    }
    if (sink_draining || sink_calls_in_flight != 0)
        return MakeError(ErrorCategory::Busy, "RemapUnderToken",
                         "a publication callback is in flight or draining");
    if (AnyPinOverlapsLocked(range)) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::RemapUnderToken",
                         "a pinned span overlaps this range; release it before remapping");
    }
    auto found = std::find_if(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base.value == range.base.value && m.range.size == range.size;
    });
    if (found == mappings.end()) {
        return MakeError(ErrorCategory::InvalidArgument,
                         "GuestAddressSpace::RemapUnderToken",
                         "no exact whole mapping at that VA to remap");
    }

    if (HasPermission(permission, GuestPermission::Execute) &&
        std::any_of(poisoned_ranges.begin(), poisoned_ranges.end(),
                    [&](GuestRange poisoned) { return RangesOverlap(range, poisoned); }))
        return MakeError(ErrorCategory::WrongState, "RemapUnderToken",
                         "cannot grant execute over poisoned code");

    // Retire the old backing's shared and per-owner translations before replacing it.
    // CallSinkUnlocked excludes all other mutations while the context lock is acquired.
    bool committed = false;
    auto invalidated = CallSinkUnlocked(guard, range, InvalidationReason::Unmap, committed);
    auto token_status = CheckTokenLocked(token, "RemapUnderToken");
    if (!committed || !token_status) {
        PoisonCodeLocked(range);
        ++code_generation;
        return !token_status ? token_status.GetError() : invalidated.GetError();
    }
    // No unlocked interval between the successful invalidation and MAP_FIXED.
    void* result = SeamMmap(HostPointer(range.base), static_cast<std::size_t>(range.size),
                            ToHostProtection(permission),
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        const int saved = errno;
        // Do not infer that MAP_FIXED failure preserved every old page. Keep execution closed
        // until a successful remap/invalidation repairs this range; fault injection is required
        // before claiming stronger rollback semantics.
        PoisonCodeLocked(range);
        ++code_generation;
        auto error = MakeError(CategoriseMapFailure(saved),
                               "GuestAddressSpace::RemapUnderToken", "remap mmap failed; backing requires recovery");
        error.system_error = saved;
        return error;
    }
    ++code_generation;
    ClearPoisonIfRepairedLocked(range);
    ++mapping_generation;
    found->permission = permission;
    found->generation = mapping_generation;

    MappingInfo info{};
    info.range = range;
    info.permission = permission;
    info.mapping_generation = mapping_generation;
    info.host_owned = true;
    return info;
}

Status GuestAddressSpace::UpdateVmUnderToken(const QuiescenceToken& token, VmOperation operation,
                                             GuestRange range, GuestPermission permission, int fd,
                                             std::uint64_t offset) {
    constexpr auto name = "GuestAddressSpace::UpdateVmUnderToken";
    auto checked = GuestRange::Checked(range.base, range.size);
    if (!checked || !IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size) ||
        (operation != VmOperation::Map && operation != VmOperation::Protect &&
         operation != VmOperation::Unmap) ||
        fd < -1 || (fd == -1 && offset != 0) || !OwnsRange(range) || !IsHostPageAligned(offset) ||
        offset > INT64_MAX || range.size > INT64_MAX - offset ||
        (operation != VmOperation::Map && (fd != -1 || offset != 0)))
        return MakeError(ErrorCategory::InvalidArgument, name, "invalid VM range/backing");
    std::unique_lock guard{lock};
    if (auto s = CheckTokenLocked(token, name); !s)
        return s;
    if (sink_draining || sink_calls_in_flight || !pins.empty())
        return MakeError(ErrorCategory::Busy, name, "VM requires drained pins and sink");
    if (!poisoned_ranges.empty())
        return MakeError(ErrorCategory::WrongState, name,
                         "generation requires recovery after VM failure");

    // Build the complete post-operation ledger before changing any host mapping.
    // Allocation failure leaves the old ledger and backing intact.
    std::vector<Mapping> updated;
    updated.reserve(mappings.size() + 2);
    std::uint64_t covered{};
    for (auto m : mappings) {
        if (!RangesOverlap(m.range, range)) {
            updated.push_back(m);
            continue;
        }
        const auto begin = std::max(m.range.base.value, range.base.value);
        const auto end = std::min(m.range.End(), range.End());
        covered += end - begin;
        if (m.range.base.value < begin) {
            auto left = m;
            left.range.size = begin - left.range.base.value;
            updated.push_back(left);
        }
        if (operation == VmOperation::Protect) {
            auto middle = m;
            middle.range = {GuestAddress{begin}, end - begin};
            middle.permission = permission;
            middle.generation = mapping_generation + 1;
            updated.push_back(middle);
        }
        if (end < m.range.End()) {
            auto right = m;
            right.range = {GuestAddress{end}, m.range.End() - end};
            updated.push_back(right);
        }
    }
    if (operation == VmOperation::Protect && covered != range.size)
        return MakeError(ErrorCategory::InvalidArgument, name,
                         "protection crosses unmapped memory");
    if (operation == VmOperation::Map)
        updated.push_back(
            Mapping{range, permission, ProtectionReason::GuestPermission, mapping_generation + 1});

    // Full retirement also covers partial and differently placed shared backing aliases.
    // FEX walks its sparse compiled-page keys here, not every reserved host page.
    const GuestRange all{reservation_base, reservation_size};
    bool committed = false;
    auto retired = CallSinkUnlocked(guard, all, InvalidationReason::FullFlush, committed);
    if (!committed) {
        PoisonCodeLocked(all);
        ++code_generation;
        return retired;
    }
    if (auto s = CheckTokenLocked(token, name); !s) {
        PoisonCodeLocked(all);
        return s;
    }
    bool success;
    if (operation == VmOperation::Protect) {
        success =
            SeamMprotect(HostPointer(range.base), range.size, ToHostProtection(permission)) == 0;
    } else {
        const bool backed = operation == VmOperation::Map && fd >= 0;
        const int flags = MAP_FIXED | (backed ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS);
        const auto prot = operation == VmOperation::Unmap ? GuestPermission::None : permission;
        success = SeamMmap(HostPointer(range.base), range.size, ToHostProtection(prot), flags,
                           backed ? fd : -1, backed ? offset : 0) != MAP_FAILED;
    }
    ++code_generation;
    if (!success) {
        const int saved = errno;
        PoisonCodeLocked(all);
        auto error =
            MakeError(CategoriseMapFailure(saved), name, "VM syscall failed; generation poisoned");
        error.system_error = saved;
        return error;
    }
    mappings.swap(updated);
    shared_backing_seen |= operation == VmOperation::Map && fd >= 0;
    ++mapping_generation;
    return Ok();
}

// Requires lock. Poison clears only when the range that failed has itself been republished or
// invalidated successfully. Clearing on any successful publication anywhere would let an unrelated
// range re-enable execution of the still-stale one.
void GuestAddressSpace::ClearPoisonIfRepairedLocked(GuestRange repaired) {
    const auto repaired_end = repaired.base.value + repaired.size;
    std::erase_if(poisoned_ranges, [&](GuestRange poisoned) {
        return repaired.base.value <= poisoned.base.value &&
               poisoned.base.value + poisoned.size <= repaired_end;
    });
}

void GuestAddressSpace::PoisonCodeLocked(GuestRange range) {
    RevokeExecuteLocked(range);
    // Keep each failed interval until a repair covers it; a second failure must
    // never replace the first one's recovery obligation.
    if (std::none_of(poisoned_ranges.begin(), poisoned_ranges.end(), [&](GuestRange known) {
            return known.base == range.base && known.size == range.size;
        })) {
        poisoned_ranges.push_back(range);
    }
}

Status GuestAddressSpace::CheckMappingMutationLocked(std::string_view operation) const {
    if (active_quiescence != 0 || execution_leases != 0 || sink_calls_in_flight != 0 || sink_draining) {
        return MakeError(ErrorCategory::Busy, operation,
                         "mapping mutation requires an idle address space outside publication");
    }
    return Ok();
}

Status GuestAddressSpace::RegisterAlias(GuestRange primary, GuestAddress alias_base) {
    std::lock_guard guard{lock};
    if (auto status = CheckMappingMutationLocked("GuestAddressSpace::RegisterAlias"); !status) {
        return status;
    }
    const bool known = std::any_of(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base.value <= primary.base.value &&
               primary.base.value + primary.size <= m.range.base.value + m.range.size;
    });
    if (!known) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::RegisterAlias",
                         "primary range is not mapped");
    }
    aliases.push_back(Alias{primary, alias_base});
    ++mapping_generation;
    return Ok();
}

void GuestAddressSpace::AddObserver(MemoryObserver* observer) {
    if (observer == nullptr) {
        return;
    }
    std::lock_guard guard{lock};
    if (std::find(observers.begin(), observers.end(), observer) == observers.end()) {
        observers.push_back(observer);
    }
}

void GuestAddressSpace::RemoveObserver(MemoryObserver* observer) {
    std::lock_guard guard{lock};
    std::erase(observers, observer);
}

void GuestAddressSpace::NotifyObservers(GuestRange range) {
    // Widen to whole host pages before reporting. Over-reporting a dirty range
    // is acceptable; missing a notification is not (acceptance M12).
    GuestRange widened{};
    widened.base = GuestAddress{AlignDownToHostPage(range.base.value)};
    const std::uint64_t end = AlignUpToHostPage(range.base.value + range.size);
    widened.size = end > widened.base.value ? end - widened.base.value : range.size;

    std::vector<MemoryObserver*> snapshot;
    {
        std::lock_guard guard{lock};
        snapshot = observers;
    }
    // Called outside the lock: an observer must never be able to deadlock the
    // address space by calling back into it.
    for (auto* observer : snapshot) {
        observer->OnGuestWrite(widened);
    }
}

GuestAddressSpace::ResourceCounts GuestAddressSpace::Counts() const {
    std::lock_guard guard{lock};
    return ResourceCounts{mappings.size(), aliases.size(), pins.size(), observers.size()};
}

} // namespace Core::GuestCpu
