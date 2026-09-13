// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <boost/icl/separate_interval_set.hpp>
#include "common/arch.h"
#include "common/enum.h"
#include "common/types.h"

namespace Core {

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

// Injected production guest VM. Desktop keeps its native implementation;
// Android uses the same MemoryManager against the FEX-owned reservation.
class GuestMemoryBackend {
public:
    virtual ~GuestMemoryBackend() = default;
    virtual u8* BackingBase() const = 0;
    virtual boost::icl::interval_set<VAddr> UsableRegions() const = 0;
    virtual bool OwnsRange(VAddr address, u64 size) const = 0;
    virtual void* Map(VAddr address, u64 size, PAddr physical, bool executable) = 0;
    virtual void* MapFile(VAddr address, u64 size, u64 offset, u32 prot, uintptr_t fd) = 0;
    virtual void Unmap(VAddr address, u64 size) = 0;
    virtual void Protect(VAddr address, u64 size, MemoryPermission permission) = 0;
    virtual void ProtectGpu(VAddr address, u64 size, MemoryPermission permission) {
        Protect(address, size, permission);
    }
    virtual bool IsGpuWatchFault(VAddr address, bool write) const {
        return false;
    }
};

/**
 * Represents the user virtual address space backed by a dmem memory block
 */
class AddressSpace {
public:
    void ProtectGpu(VAddr address, u64 size, MemoryPermission permission) {
        if (guest)
            guest->ProtectGpu(address, size, permission);
        else
            Protect(address, size, permission);
    }
    bool IsGuestBackend() const {
        return guest != nullptr;
    }
    bool IsGpuWatchFault(VAddr address, bool write) const {
        return guest && guest->IsGpuWatchFault(address, write);
    }
    explicit AddressSpace();
    explicit AddressSpace(GuestMemoryBackend* guest);
    ~AddressSpace();

    [[nodiscard]] u8* BackingBase() const noexcept {
        return backing_base;
    }

    [[nodiscard]] VAddr SystemManagedVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(system_managed_base);
    }
    [[nodiscard]] const u8* SystemManagedVirtualBase() const noexcept {
        return system_managed_base;
    }
    [[nodiscard]] u64 SystemManagedVirtualSize() const noexcept {
        return system_managed_size;
    }

    [[nodiscard]] VAddr SystemReservedVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(system_reserved_base);
    }
    [[nodiscard]] const u8* SystemReservedVirtualBase() const noexcept {
        return system_reserved_base;
    }
    [[nodiscard]] u64 SystemReservedVirtualSize() const noexcept {
        return system_reserved_size;
    }

    [[nodiscard]] VAddr UserVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(user_base);
    }
    [[nodiscard]] const u8* UserVirtualBase() const noexcept {
        return user_base;
    }
    [[nodiscard]] u64 UserVirtualSize() const noexcept {
        return user_size;
    }

    /**
     * @brief Maps memory to the specified virtual address.
     * @param virtual_addr The base address to place the mapping.
     *        If zero is provided an address in system managed area is picked.
     * @param size The size of the area to map.
     * @param phys_addr The offset of the backing file handle to map.
     *                  The same backing region may be aliased into different virtual regions.
     *                  If zero is provided the mapping is considered as private.
     * @return A pointer to the mapped memory.
     */
    void* Map(VAddr virtual_addr, u64 size, PAddr phys_addr = -1, bool exec = false);

    /// Memory maps a specified file descriptor.
    void* MapFile(VAddr virtual_addr, u64 size, u64 offset, u32 prot, uintptr_t fd);

    /// Unmaps specified virtual memory area.
    void Unmap(VAddr virtual_addr, u64 size);

    /// Protects requested region.
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);

    // Returns an interval set containing all usable regions.
    boost::icl::interval_set<VAddr> GetUsableRegions();
    bool OwnsGuestRange(VAddr address, u64 size) const {
        return !guest || guest->OwnsRange(address, size);
    }

private:
    GuestMemoryBackend* guest{};
    struct Impl;
    std::unique_ptr<Impl> impl;
    u8* backing_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
};

} // namespace Core
