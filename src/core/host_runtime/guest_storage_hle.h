// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include "core/guest_cpu/api/address_space.h"
#include "guest_storage.h"
namespace Core::HostRuntime {
enum class StorageOp {
    UnmountBackup,
    CheckBackup,
    RestoreBackup,
    Delete,
    Event,
    Init,
    Term,
    Mount,
    Mount2,
    Unmount,
    Info,
    GetParam,
    SetParam,
    Open,
    Close,
    Read,
    Write,
    Seek,
    Sync,
    Mkdir,
    Unlink,
    Rename
};
struct StorageEntry {
    std::string_view nid;
    StorageOp op;
    bool save;
    bool posix{};
};
inline constexpr StorageEntry StorageEntries[]{
    {"VwadwBBBJ80", StorageOp::UnmountBackup, true}, {"RQOqDbk3bSU", StorageOp::CheckBackup, true},
    {"lU9YRFsgwSU", StorageOp::RestoreBackup, true}, {"S1GkePI17zQ", StorageOp::Delete, true},
    {"j8xKtiFj0SY", StorageOp::Event, true},         {"ZkZhskCPXFw", StorageOp::Init, true},
    {"l1NmDeDpNGU", StorageOp::Init, true},          {"TywrFKCoLGY", StorageOp::Init, true},
    {"yKDy8S5yLA0", StorageOp::Term, true},          {"32HQAQdwM2o", StorageOp::Mount, true},
    {"0z45PIH+SNI", StorageOp::Mount2, true},        {"BMR4F-Uek3E", StorageOp::Unmount, true},
    {"65VH0Qaaz6s", StorageOp::Info, true},          {"XgvSuIdnMlw", StorageOp::GetParam, true},
    {"85zul--eGXs", StorageOp::SetParam, true},      {"1G3lF1Gg1k8", StorageOp::Open, false},
    {"6c3rCVE-fTU", StorageOp::Open, false, true},   {"wuCroIGjt2g", StorageOp::Open, false, true},
    {"UK2Tl2DWUns", StorageOp::Close, false},        {"NNtFaKJbPt0", StorageOp::Close, false, true},
    {"bY-PO6JhzhQ", StorageOp::Close, false, true},  {"Cg4srZ6TKbU", StorageOp::Read, false},
    {"DRuBt2pvICk", StorageOp::Read, false, true},   {"AqBioC2vF3I", StorageOp::Read, false, true},
    {"4wSze92BhLI", StorageOp::Write, false},        {"FxVZqBAA7ks", StorageOp::Write, false, true},
    {"FN4gaPmuFV8", StorageOp::Write, false, true},  {"oib76F-12fk", StorageOp::Seek, false},
    {"Oy6IpwgtYOk", StorageOp::Seek, false, true},   {"fTx66l5iWIA", StorageOp::Sync, false},
    {"1-LFLmRFxxM", StorageOp::Mkdir, false},        {"JGMio+21L4c", StorageOp::Mkdir, false, true},
    {"AUXVxWeJU-A", StorageOp::Unlink, false},       {"52NcYU9+lEo", StorageOp::Rename, false}};
u64 DispatchStorage(GuestStorage& storage, GuestCpu::GuestAddressSpace& space,
                    const StorageEntry& entry, const std::array<u64, 6>& args,
                    const std::function<u64(int)>& posix_failure);
} // namespace Core::HostRuntime
