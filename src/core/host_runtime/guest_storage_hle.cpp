// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#include "core/libraries/kernel/kernel.h"
#include "guest_storage_hle.h"
namespace Core::HostRuntime {
using namespace GuestCpu;
namespace {
struct Mount2 {
    s32 user;
    u32 pad;
    u64 directory, blocks;
    u32 mode;
    std::array<u8, 36> reserved;
};
struct Mount1 {
    s32 user;
    u32 pad;
    u64 title, directory, fingerprint, blocks;
    u32 mode;
    std::array<u8, 36> reserved;
};
static_assert(sizeof(Mount2) == 64 && sizeof(Mount1) == 80);
template <class T>
bool Copy(GuestAddressSpace& space, u64 address, T& value) {
    return bool(space.Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
}
std::optional<std::string> String(GuestAddressSpace& space, u64 address, size_t max) {
    std::string out;
    if (!address || address > UINT64_MAX - max)
        return {};
    for (size_t i = 0; i < max; ++i) {
        char c{};
        if (!Copy(space, address + i, c))
            return {};
        if (!c)
            return out;
        out += c;
    }
    return {};
}
} // namespace
u64 DispatchStorage(GuestStorage& storage, GuestAddressSpace& space, const StorageEntry& e,
                    const std::array<u64, 6>& a, const std::function<u64(int)>& posix_failure) {
    using SE = GuestStorage::Error;
    auto error = [&](int native) -> u64 {
        if (e.save)
            return static_cast<u32>(SE::PARAMETER);
        int posix = Libraries::Kernel::NativeToPosixErrno(native);
        return e.posix ? posix_failure(posix)
                       : static_cast<u64>(s64(Libraries::Kernel::ErrnoToSceKernelError(posix)));
    };
    auto io = [&](GuestStorage::IoResult r) -> u64 {
        return r.error ? error(r.error) : u64(r.value);
    };
    switch (e.op) {
    case StorageOp::Event: {
        auto pin = space.AcquirePinnedSpan({GuestAddress{a[1]}, sizeof(GuestStorage::Event)}, true);
        if (!pin)
            return error(EFAULT);
        GuestStorage::Event value{};
        auto r = storage.GetEvent(value);
        if (r == SE::OK)
            std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        return u32(r);
    }
    case StorageOp::CheckBackup:
    case StorageOp::RestoreBackup:
    case StorageOp::Delete: {
        struct Input {
            s32 user;
            u32 pad;
            u64 title, directory, param, icon;
            std::array<u8, 32> reserved;
        };
        Input in{};
        // Delete/Restore are 72 bytes; CheckBackup is 72 bytes with nested outputs.
        if (!space.Read(GuestAddress{a[0]}, std::as_writable_bytes(std::span{&in, 1})
                                                .first(e.op == StorageOp::Delete ? 64 : 72)))
            return error(EFAULT);
        auto dir = String(space, in.directory, 32);
        if (!dir)
            return error(EFAULT);
        std::string title;
        if (in.title) {
            auto t = String(space, in.title, 10);
            if (!t)
                return error(EFAULT);
            title = *t;
        }
        if (e.op == StorageOp::Delete)
            return u32(storage.Delete(in.user, title, *dir));
        if (e.op == StorageOp::RestoreBackup) {
            std::array<u8, 80> fingerprint{};
            if (in.param && !Copy(space, in.param, fingerprint))
                return error(EFAULT);
            return u32(storage.RestoreBackup(in.user, title, *dir));
        }
        std::optional<PinnedSpan> param, icon_out, icon_data;
        struct Icon {
            u64 buffer, capacity, size;
            std::array<u8, 32> reserved;
        };
        Icon icon{};
        if (in.param) {
            auto p = space.AcquirePinnedSpan(
                {GuestAddress{in.param}, sizeof(Libraries::SaveData::OrbisSaveDataParam)}, true);
            if (!p)
                return error(EFAULT);
            param = std::move(p).Value();
        }
        if (in.icon) {
            if (!Copy(space, in.icon, icon) || icon.capacity > 4 * 1024 * 1024)
                return error(EFAULT);
            auto p = space.AcquirePinnedSpan({GuestAddress{in.icon}, sizeof(icon)}, true);
            if (!p)
                return error(EFAULT);
            icon_out = std::move(p).Value();
            if (icon.capacity) {
                auto b = space.AcquirePinnedSpan({GuestAddress{icon.buffer}, icon.capacity}, true);
                if (!b)
                    return error(EFAULT);
                icon_data = std::move(b).Value();
            }
        }
        Libraries::SaveData::OrbisSaveDataParam metadata{};
        std::vector<u8> bytes;
        auto r = storage.CheckBackup(in.user, title, *dir, metadata, bytes);
        if (r == SE::OK) {
            if (param)
                std::memcpy(param->WritableBytes().data(), &metadata, sizeof(metadata));
            if (icon_out) {
                icon.size = bytes.size();
                std::memcpy(icon_out->WritableBytes().data(), &icon, sizeof(icon));
                if (icon_data)
                    std::memcpy(icon_data->WritableBytes().data(), bytes.data(),
                                std::min<u64>(bytes.size(), icon.capacity));
            }
        }
        return u32(r);
    }
    case StorageOp::Init:
        return static_cast<u32>(storage.Initialize());
    case StorageOp::Term:
        return static_cast<u32>(storage.Terminate());
    case StorageOp::Mount:
    case StorageOp::Mount2: {
        Mount2 m{};
        std::string title;
        if (e.op == StorageOp::Mount) {
            Mount1 legacy{};
            if (!Copy(space, a[0], legacy))
                return error(EFAULT);
            m.user = legacy.user;
            m.directory = legacy.directory;
            m.blocks = legacy.blocks;
            m.mode = legacy.mode;
            if (legacy.title) {
                auto t = String(space, legacy.title, 10);
                if (!t)
                    return error(EFAULT);
                title = *t;
            }
            // Fingerprint is optional opaque input; never dereference it natively.
            std::array<u8, 80> fingerprint{};
            if (legacy.fingerprint && !Copy(space, legacy.fingerprint, fingerprint))
                return error(EFAULT);
        } else if (!Copy(space, a[0], m))
            return error(EFAULT);
        auto dir = String(space, m.directory, 32);
        if (!dir)
            return error(EFAULT);
        auto out =
            space.AcquirePinnedSpan({GuestAddress{a[1]}, sizeof(GuestStorage::MountResult)}, true);
        if (!out)
            return error(EFAULT);
        GuestStorage::MountResult value{};
        auto result = storage.Mount(m.user, title, *dir, m.blocks, m.mode, value);
        if (result == SE::OK || result == SE::NO_SPACE_FS)
            std::memcpy(out.Value().WritableBytes().data(), &value, sizeof(value));
        return static_cast<u32>(result);
    }
    case StorageOp::UnmountBackup:
    case StorageOp::Unmount:
    case StorageOp::Info:
    case StorageOp::GetParam:
    case StorageOp::SetParam: {
        auto point = String(space, a[0], 16);
        if (!point)
            return error(EFAULT);
        if (e.op == StorageOp::UnmountBackup)
            return static_cast<u32>(storage.UnmountBackup(*point));
        if (e.op == StorageOp::Unmount)
            return static_cast<u32>(storage.Unmount(*point));
        if (e.op == StorageOp::Info) {
            auto pin = space.AcquirePinnedSpan(
                {GuestAddress{a[1]}, sizeof(GuestStorage::MountInfo)}, true);
            if (!pin)
                return error(EFAULT);
            GuestStorage::MountInfo value{};
            auto result = storage.Info(*point, value);
            if (result == SE::OK)
                std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
            return static_cast<u32>(result);
        }
        if (a[3] == 0 || a[3] > sizeof(Libraries::SaveData::OrbisSaveDataParam))
            return error(EINVAL);
        if (e.op == StorageOp::SetParam) {
            std::vector<u8> local(a[3]);
            if (!space.Read(GuestAddress{a[2]}, std::as_writable_bytes(std::span{local})))
                return error(EFAULT);
            return static_cast<u32>(storage.SetParam(*point, a[1], local));
        }
        auto out = space.AcquirePinnedSpan({GuestAddress{a[2]}, a[3]}, true);
        if (!out)
            return error(EFAULT);
        std::optional<PinnedSpan> size_out;
        if (a[4]) {
            auto pin = space.AcquirePinnedSpan({GuestAddress{a[4]}, 8}, true);
            if (!pin)
                return error(EFAULT);
            size_out = std::move(pin).Value();
        }
        std::vector<u8> local(a[3]);
        u64 size{};
        auto result = storage.GetParam(*point, a[1], local, size);
        if (result == SE::OK) {
            std::memcpy(out.Value().WritableBytes().data(), local.data(), size);
            if (size_out)
                std::memcpy(size_out->WritableBytes().data(), &size, 8);
        }
        return static_cast<u32>(result);
    }
    case StorageOp::Open:
    case StorageOp::Mkdir:
    case StorageOp::Unlink:
    case StorageOp::Rename: {
        auto path = String(space, a[0], 1024);
        if (!path)
            return error(EFAULT);
        if (e.op == StorageOp::Open)
            return io(storage.Open(*path, a[1], a[2]));
        if (e.op == StorageOp::Mkdir)
            return io(storage.Mkdir(*path, a[1]));
        if (e.op == StorageOp::Unlink)
            return io(storage.Unlink(*path));
        auto to = String(space, a[1], 1024);
        if (!to)
            return error(EFAULT);
        return io(storage.Rename(*path, *to));
    }
    case StorageOp::Close:
        return io(storage.Close(s32(a[0])));
    case StorageOp::Seek:
        return io(storage.Seek(s32(a[0]), s64(a[1]), s32(a[2])));
    case StorageOp::Sync:
        return io(storage.Sync(s32(a[0])));
    case StorageOp::Read:
    case StorageOp::Write: {
        // Regular files only; bound one I/O admission so Cancel has an HLE boundary.
        if (a[2] > u64(INT64_MAX))
            return error(EINVAL);
        const auto count = std::min<u64>(a[2], 16 * 1024 * 1024);
        if (!a[2])
            return e.op == StorageOp::Read ? io(storage.Read(a[0], {}))
                                           : io(storage.Write(a[0], {}));
        auto pin = space.AcquirePinnedSpan({GuestAddress{a[1]}, count}, e.op == StorageOp::Read);
        if (!pin)
            return error(EFAULT);
        if (e.op == StorageOp::Read) {
            auto bytes = pin.Value().WritableBytes();
            return io(storage.Read(a[0], {reinterpret_cast<u8*>(bytes.data()), bytes.size()}));
        }
        auto bytes = pin.Value().Bytes();
        return io(storage.Write(a[0], {reinterpret_cast<const u8*>(bytes.data()), bytes.size()}));
    }
    }
    return error(EINVAL);
}
} // namespace Core::HostRuntime
