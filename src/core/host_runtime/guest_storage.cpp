// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "core/file_sys/fs.h"
#include "guest_storage.h"

namespace Core::HostRuntime {
namespace fs = std::filesystem;
using namespace Libraries::SaveData;
namespace {
bool Component(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find_first_of("/\\\0", 0, 3) == std::string_view::npos;
}
// The app owns the root. Reject symlinks at every level before backend metadata
// operations; guest file operations use descriptor-relative O_NOFOLLOW below.
void SafeDirectory(const fs::path& root, const fs::path& relative) {
    fs::path current = root;
    fs::create_directories(root);
    for (const auto& part : relative) {
        current /= part;
        auto status = fs::symlink_status(current);
        if (fs::is_symlink(status))
            throw fs::filesystem_error(
                "Save path is a symlink", current,
                std::make_error_code(std::errc::too_many_symbolic_link_levels));
        if (!fs::exists(status))
            fs::create_directory(current);
        else if (!fs::is_directory(status))
            throw fs::filesystem_error("Save path is not a directory", current,
                                       std::make_error_code(std::errc::not_a_directory));
    }
}
GuestStorage::Error Failure(const fs::filesystem_error& e) {
    LOG_ERROR(Lib_SaveData, "Session savedata operation failed: {}", e.what());
    if (e.code() == std::errc::no_space_on_device)
        return GuestStorage::Error::NO_SPACE_FS;
    if (e.code() == std::errc::illegal_byte_sequence)
        return GuestStorage::Error::BROKEN;
    return GuestStorage::Error::INTERNAL;
}
void SyncPath(const fs::path& path, bool directory = false) {
    int fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (fd < 0)
        throw fs::filesystem_error("Save open for sync", path,
                                   std::error_code(errno, std::generic_category()));
    int result = ::fsync(fd);
    int error = errno;
    ::close(fd);
    if (result)
        throw fs::filesystem_error("Save sync", path,
                                   std::error_code(error, std::generic_category()));
}
} // namespace
bool GuestStorage::ValidTitle(std::string_view id) {
    return id.size() == 9 &&
           std::all_of(id.begin(), id.begin() + 4, [](char c) { return c >= 'A' && c <= 'Z'; }) &&
           std::all_of(id.begin() + 4, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
GuestStorage::GuestStorage(FileSys::MntPoints& m, fs::path h, std::string t, int u)
    : mounts(m), home(std::move(h)), title(std::move(t)), user(u) {
    if (!home.is_absolute() || !ValidTitle(title) || user < 0)
        throw std::invalid_argument("invalid persistent savedata identity/root");
    // Context.filesDir may use Android's legitimate /data/user/0 symlink.
    // Resolve the host-provided root once; reject symlinks only below that root.
    home = fs::weakly_canonical(home);
}
GuestStorage::~GuestStorage() {
    for (auto [id, file] : files)
        ::close(file.host);
    files.clear();
    for (auto& slot : slots)
        if (slot) {
            auto result = UnmountLocked(slot->GetMountPoint());
            if (result != Error::OK) {
                LOG_ERROR(Lib_SaveData, "Session save retirement failed: {:#x}",
                          static_cast<u32>(result));
                if (slot)
                    slot->Abandon();
            }
        }
}
GuestStorage::Error GuestStorage::Initialize() {
    std::lock_guard lock(mutex);
    try {
        SafeDirectory(home, fs::path(std::to_string(user)) / "savedata" / title);
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
    initialized = true;
    return Error::OK;
}
GuestStorage::Error GuestStorage::Terminate() {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (std::any_of(slots.begin(), slots.end(), [](const auto& s) { return bool(s); }))
        return Error::BUSY;
    initialized = false;
    return Error::OK;
}
u64 GuestStorage::Used(const fs::path& root) {
    u64 used{};
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator();
         ++it) {
        const auto& e = *it;
        if (it.depth() == 0 && e.path().filename().string().starts_with("sce_backup")) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_symlink())
            throw fs::filesystem_error(
                "Symlink in save", e.path(),
                std::make_error_code(std::errc::too_many_symbolic_link_levels));
        if (e.is_regular_file())
            used += e.file_size();
    }
    return used;
}
GuestStorage::Error GuestStorage::Mount(int uid, std::string_view tid, std::string_view dir,
                                        u64 blocks, u32 mode, MountResult& result) {
    std::lock_guard lock(mutex);
    result = {};
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (uid != user)
        return Error::INVALID_LOGIN_USER;
    if ((!tid.empty() && tid != title) || !Component(dir) || dir.starts_with(".") ||
        dir.size() >= 32 || (mode & ~0x3fu) || ((mode & 3) != 1 && (mode & 3) != 2) ||
        ((mode & 4) && (mode & 32)) || ((mode & 1) && (mode & (4 | 32))))
        return Error::PARAMETER;
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (slots[i] && slots[i]->GetDirName() == dir)
            return Error::BUSY;
        if (!slots[i] && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return Error::MOUNT_FULL;
    const auto path = home / std::to_string(user) / "savedata" / title / dir;
    bool created_directory = false;
    try {
        // An existing partial/corrupt save must never be recreated over in place.
        bool existed = fs::exists(path);
        if (fs::is_symlink(fs::symlink_status(path)))
            return Error::BROKEN;
        if (!existed && !(mode & (4 | 32)))
            return Error::NOT_FOUND;
        if (existed && (mode & 4))
            return Error::EXISTS;
        if (!existed && (blocks < 96 || blocks > 32768))
            return Error::PARAMETER;
        if (existed && !fs::exists(Save::GetParamSFOPath(path)))
            return Error::BROKEN;
        SafeDirectory(home, path.lexically_relative(home));
        created_directory = !existed;
        // Also reject symlink metadata and files before opening the save backend.
        (void)Used(path);
        if (!existed) {
            const u64 available = fs::space(path).available;
            if (available < blocks * 32768) {
                result.required_blocks = (blocks * 32768 - available + 32767) / 32768;
                fs::remove(path);
                return Error::NO_SPACE_FS;
            }
        }
        auto instance = std::make_unique<Save>(
            slot, uid, title, dir, static_cast<int>(std::min<u64>(blocks, 32768)), &mounts, path);
        instance->SetupAndMount(mode & 1, mode & 16, mode & 8, true);
        std::copy(instance->GetMountPoint().begin(), instance->GetMountPoint().end(),
                  result.point.begin());
        result.status = !existed && (mode & 32) ? 1 : 0;
        slots[slot] = std::move(instance);
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        if (created_directory) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
        return Failure(e);
    }
}
GuestStorage::Save* GuestStorage::Find(std::string_view point) {
    for (auto& slot : slots)
        if (slot && slot->GetMountPoint() == point)
            return slot.get();
    return nullptr;
}
GuestStorage::Error GuestStorage::UnmountLocked(std::string_view point) {
    for (int i = 0; i < 16; ++i)
        if (slots[i] && slots[i]->GetMountPoint() == point) {
            for (const auto& [id, f] : files)
                if (f.slot == i)
                    return Error::BUSY;
            try {
                if (!slots[i]->IsReadOnly()) {
                    for (const auto& e :
                         fs::recursive_directory_iterator(slots[i]->GetSavePath())) {
                        if (e.is_regular_file())
                            SyncPath(e.path());
                    }
                }
                slots[i]->Umount();
                slots[i].reset();
                return Error::OK;
            } catch (const fs::filesystem_error& e) {
                return Failure(e);
            }
        }
    return Error::NOT_MOUNTED;
}
GuestStorage::Error GuestStorage::Unmount(std::string_view point) {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    return UnmountLocked(point);
}
GuestStorage::Error GuestStorage::Info(std::string_view point, MountInfo& result) {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* s = Find(point);
    if (!s)
        return Error::NOT_MOUNTED;
    try {
        result = {};
        result.blocks = s->GetMaxBlocks();
        auto used = (Used(s->GetSavePath()) + 32767) / 32768;
        result.free_blocks = result.blocks > used ? result.blocks - used : 0;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
    return Error::OK;
}
GuestStorage::Error GuestStorage::GetParam(std::string_view point, u32 type, std::span<u8> out,
                                           u64& size) {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* s = Find(point);
    if (!s)
        return Error::NOT_MOUNTED;
    OrbisSaveDataParam param{};
    param.FromSFO(s->GetParamSFO());
    const void* data{};
    switch (type) {
    case 0:
        data = &param;
        size = sizeof(param);
        break;
    case 1:
        data = &param.title;
        size = sizeof(param.title);
        break;
    case 2:
        data = &param.subTitle;
        size = sizeof(param.subTitle);
        break;
    case 3:
        data = &param.detail;
        size = sizeof(param.detail);
        break;
    case 4:
        data = &param.userParam;
        size = sizeof(param.userParam);
        break;
    case 5:
        data = &param.mtime;
        size = sizeof(param.mtime);
        break;
    default:
        return Error::PARAMETER;
    }
    if (out.size() < size)
        return Error::PARAMETER;
    std::memcpy(out.data(), data, size);
    return Error::OK;
}
GuestStorage::Error GuestStorage::SetParam(std::string_view point, u32 type,
                                           std::span<const u8> in) {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* s = Find(point);
    if (!s)
        return Error::NOT_MOUNTED;
    if (s->IsReadOnly())
        return Error::BAD_MOUNTED;
    OrbisSaveDataParam param{};
    param.FromSFO(s->GetParamSFO());
    void* data{};
    size_t size{};
    switch (type) {
    case 0:
        data = &param;
        size = sizeof(param);
        break;
    case 1:
        data = &param.title;
        size = sizeof(param.title);
        break;
    case 2:
        data = &param.subTitle;
        size = sizeof(param.subTitle);
        break;
    case 3:
        data = &param.detail;
        size = sizeof(param.detail);
        break;
    case 4:
        data = &param.userParam;
        size = sizeof(param.userParam);
        break;
    default:
        return Error::PARAMETER;
    }
    if (in.size() != size)
        return Error::PARAMETER;
    std::memcpy(data, in.data(), size);
    if (!std::memchr(&param.title, 0, sizeof(param.title)) ||
        !std::memchr(&param.subTitle, 0, sizeof(param.subTitle)) ||
        !std::memchr(&param.detail, 0, sizeof(param.detail)))
        return Error::PARAMETER;
    param.ToSFO(s->GetParamSFO());
    return Error::OK;
}
// Walk from the selected mount root with O_NOFOLLOW. No host path, symlink,
// '..', or stale /savedataN handle can escape into the host application's files.
GuestStorage::Parent GuestStorage::Resolve(std::string_view path, bool write) {
    Parent p;
    if (path.empty() || path.front() != '/' || path.find('\0') != path.npos) {
        p.error = EINVAL;
        return p;
    }
    const auto slash = path.find('/', 1);
    if (slash == path.npos || slash + 1 == path.size()) {
        p.error = EINVAL;
        return p;
    }
    auto mount = path.substr(0, slash);
    fs::path root;
    for (int i = 0; i < 16; ++i)
        if (slots[i] && slots[i]->GetMountPoint() == mount) {
            p.slot = i;
            root = slots[i]->GetSavePath();
            if (write && slots[i]->IsReadOnly()) {
                p.error = EROFS;
                return p;
            }
        }
    if (p.slot < 0) {
        if (mount != "/app0") {
            p.error = ENOENT;
            return p;
        }
        if (write) {
            p.error = EROFS;
            return p;
        }
        const auto* m = mounts.GetMount("/app0");
        if (!m) {
            p.error = ENOENT;
            return p;
        }
        root = m->host_path;
    }
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        p.error = errno;
        return p;
    }
    auto tail = path.substr(slash + 1);
    while (true) {
        auto next = tail.find('/');
        auto part = tail.substr(0, next);
        if (!Component(part) ||
            (p.slot >= 0 && (part == "sce_sys" || part.starts_with("sce_backup")))) {
            ::close(fd);
            p.error = EACCES;
            return p;
        }
        if (next == tail.npos) {
            p.fd = fd;
            p.leaf = part;
            return p;
        }
        int child = ::openat(fd, std::string(part).c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int error = errno;
        ::close(fd);
        if (child < 0) {
            p.error = error;
            return p;
        }
        fd = child;
        tail.remove_prefix(next + 1);
    }
}
GuestStorage::IoResult GuestStorage::Open(std::string_view path, u32 flags, u32 mode) {
    std::lock_guard lock(mutex);
    constexpr u32 allowed = 3 | 8 | 0x80 | 0x200 | 0x400 | 0x800;
    if ((flags & ~allowed) || (flags & 3) == 3)
        return {-1, EINVAL};
    bool write = (flags & 3) != 0;
    if (!write && (flags & (0x200 | 0x400 | 8)))
        return {-1, EINVAL};
    auto p = Resolve(path, write);
    if (p.error)
        return {-1, p.error};
    int native = (flags & 3) == 2 ? O_RDWR : (write ? O_WRONLY : O_RDONLY);
    if (flags & 8)
        native |= O_APPEND;
    if (flags & 0x80)
        native |= O_SYNC;
    if (flags & 0x200)
        native |= O_CREAT;
    if (flags & 0x400)
        native |= O_TRUNC;
    if (flags & 0x800)
        native |= O_EXCL;
    int fd =
        ::openat(p.fd, p.leaf.c_str(), native | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, mode & 0777);
    int error = errno;
    ::close(p.fd);
    if (fd < 0)
        return {-1, error};
    struct stat st {};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return {-1, EACCES};
    }
    if (next_fd == std::numeric_limits<int>::max()) {
        ::close(fd);
        return {-1, EMFILE};
    }
    int id = next_fd++;
    files.emplace(id, File{fd, p.slot, write, bool(flags & 8)});
    return {id, 0};
}
GuestStorage::IoResult GuestStorage::Close(int fd) {
    std::lock_guard lock(mutex);
    auto it = files.find(fd);
    if (it == files.end())
        return {-1, EBADF};
    int host = it->second.host;
    files.erase(it);
    int r = ::close(host);
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Read(int fd, std::span<u8> data) {
    std::lock_guard lock(mutex);
    auto it = files.find(fd);
    if (it == files.end())
        return {-1, EBADF};
    auto r = ::read(it->second.host, data.data(), data.size());
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Write(int fd, std::span<const u8> data) {
    std::lock_guard lock(mutex);
    auto it = files.find(fd);
    if (it == files.end() || !it->second.writable)
        return {-1, EBADF};
    auto& f = it->second;
    struct stat st {};
    if (::fstat(f.host, &st))
        return {-1, errno};
    auto offset = f.append ? st.st_size : ::lseek(f.host, 0, SEEK_CUR);
    if (offset < 0)
        return {-1, errno};
    if (f.slot >= 0) {
        try {
            auto used = Used(slots[f.slot]->GetSavePath());
            auto cap = u64(slots[f.slot]->GetMaxBlocks()) * 32768;
            u64 end = u64(offset) + data.size();
            if (end < u64(offset) || end > cap ||
                (end > u64(st.st_size) && end - u64(st.st_size) > cap - std::min(cap, used)))
                return {-1, ENOSPC};
        } catch (const fs::filesystem_error&) {
            return {-1, EIO};
        }
    }
    auto r = ::write(f.host, data.data(), data.size());
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Seek(int fd, s64 offset, int whence) {
    std::lock_guard lock(mutex);
    auto it = files.find(fd);
    if (it == files.end())
        return {-1, EBADF};
    if (whence < 0 || whence > 2)
        return {-1, EINVAL};
    auto r = ::lseek(it->second.host, offset, whence);
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Sync(int fd) {
    std::lock_guard lock(mutex);
    auto it = files.find(fd);
    if (it == files.end())
        return {-1, EBADF};
    auto r = ::fsync(it->second.host);
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Mkdir(std::string_view path, u32 mode) {
    std::lock_guard lock(mutex);
    auto p = Resolve(path, true);
    if (p.error)
        return {-1, p.error};
    auto r = ::mkdirat(p.fd, p.leaf.c_str(), mode & 0777);
    int e = errno;
    ::close(p.fd);
    return {r, r < 0 ? e : 0};
}
GuestStorage::IoResult GuestStorage::Unlink(std::string_view path) {
    std::lock_guard lock(mutex);
    auto p = Resolve(path, true);
    if (p.error)
        return {-1, p.error};
    auto r = ::unlinkat(p.fd, p.leaf.c_str(), 0);
    int e = errno;
    ::close(p.fd);
    return {r, r < 0 ? e : 0};
}
GuestStorage::IoResult GuestStorage::Rename(std::string_view from, std::string_view to) {
    std::lock_guard lock(mutex);
    auto a = Resolve(from, true);
    if (a.error)
        return {-1, a.error};
    auto b = Resolve(to, true);
    if (b.error) {
        ::close(a.fd);
        return {-1, b.error};
    }
    int r = -1, e = EXDEV;
    if (a.slot == b.slot) {
        r = ::renameat(a.fd, a.leaf.c_str(), b.fd, b.leaf.c_str());
        e = errno;
    }
    ::close(a.fd);
    ::close(b.fd);
    return {r, r < 0 ? e : 0};
}

namespace {
void PublishDirectory(const fs::path& pending, const fs::path& destination) {
    if (!fs::exists(destination)) {
        fs::rename(pending, destination);
        return;
    }
#if defined(__linux__)
    // Same-filesystem atomic exchange leaves the previous version recoverable
    // at pending until publication and parent-directory fsync have succeeded.
    if (::syscall(SYS_renameat2, AT_FDCWD, pending.c_str(), AT_FDCWD, destination.c_str(), 2) != 0)
        throw fs::filesystem_error("atomic savedata exchange", pending, destination,
                                   std::error_code(errno, std::generic_category()));
#else
    throw fs::filesystem_error("atomic savedata exchange unavailable", pending,
                               std::make_error_code(std::errc::operation_not_supported));
#endif
}
} // namespace
GuestStorage::Error GuestStorage::CheckIdentity(int uid, std::string_view tid,
                                                std::string_view directory) {
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (uid != user)
        return Error::INVALID_LOGIN_USER;
    if ((!tid.empty() && tid != title) || !Component(directory) || directory.starts_with(".") ||
        directory.size() >= 32)
        return Error::PARAMETER;
    for (const auto& s : slots)
        if (s && s->GetDirName() == directory)
            return Error::BUSY;
    return Error::OK;
}
void GuestStorage::CopyTree(const fs::path& from, const fs::path& to) {
    if (cancelled)
        throw fs::filesystem_error("Save operation cancelled", from,
                                   std::make_error_code(std::errc::operation_canceled));
    const auto state = fs::symlink_status(from);
    if (fs::is_symlink(state))
        throw fs::filesystem_error("Save symlink", from,
                                   std::make_error_code(std::errc::too_many_symbolic_link_levels));
    if (fs::is_directory(state)) {
        fs::create_directory(to);
        for (const auto& e : fs::directory_iterator(from)) {
            auto name = e.path().filename().string();
            if (name.starts_with("sce_backup") || name == "corrupted" ||
                name == "param.sfo.pending")
                continue;
            CopyTree(e.path(), to / e.path().filename());
        }
        SyncPath(to, true);
        return;
    }
    if (!fs::is_regular_file(state))
        throw fs::filesystem_error("Save special file", from,
                                   std::make_error_code(std::errc::invalid_argument));
    Common::FS::IOFile input(from, Common::FS::FileAccessMode::Read);
    Common::FS::IOFile output(to, Common::FS::FileAccessMode::Create);
    std::array<u8, 65536> buffer{};
    u64 remaining = input.GetSize();
    if (!input.IsOpen() || !output.IsOpen())
        throw fs::filesystem_error("Save copy open", from,
                                   std::make_error_code(std::errc::io_error));
    while (remaining) {
        if (cancelled)
            throw fs::filesystem_error("Save operation cancelled", from,
                                       std::make_error_code(std::errc::operation_canceled));
        size_t count = std::min<u64>(remaining, buffer.size());
        if (input.ReadRaw<u8>(buffer.data(), count) != count ||
            output.WriteRaw<u8>(buffer.data(), count) != count)
            throw fs::filesystem_error("Save short copy", from, to,
                                       std::make_error_code(std::errc::io_error));
        remaining -= count;
    }
    output.Close();
    if (fs::file_size(to) != input.GetSize())
        throw fs::filesystem_error("Save incomplete flush", to,
                                   std::make_error_code(std::errc::io_error));
    SyncPath(to);
}
GuestStorage::Error GuestStorage::UnmountBackup(std::string_view point) {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* save = Find(point);
    if (!save)
        return Error::NOT_MOUNTED;
    const auto path = save->GetSavePath();
    const auto directory = save->GetDirName();
    if (events.size() >= 64)
        return Error::BUSY;
    auto status = UnmountLocked(point);
    if (status != Error::OK)
        return status;
    try {
        const auto pending = path / "sce_backup_tmp";
        // Only reserved scratch, never the last committed backup.
        fs::remove_all(pending);
        CopyTree(path, pending);
        PublishDirectory(pending, path / "sce_backup");
        SyncPath(path, true);
        fs::remove_all(pending);
    } catch (const fs::filesystem_error& e) {
        status = Failure(e);
    }
    Event event{};
    event.type = 1;
    event.error = u32(status);
    event.user = user;
    event.title.data.FromString(title);
    event.directory.data.FromString(directory);
    events.push_back(event);
    return status;
}
GuestStorage::Error GuestStorage::CheckBackup(int uid, std::string_view tid,
                                              std::string_view directory, OrbisSaveDataParam& param,
                                              std::vector<u8>& icon) {
    std::lock_guard lock(mutex);
    auto status = CheckIdentity(uid, tid, directory);
    if (status != Error::OK)
        return status;
    try {
        const auto path =
            home / std::to_string(user) / "savedata" / title / directory / "sce_backup";
        if (!fs::exists(path))
            return Error::NOT_FOUND;
        (void)Used(path);
        PSF psf;
        if (!psf.Open(Save::GetParamSFOPath(path)))
            return Error::BROKEN;
        param.FromSFO(psf);
        const auto icon_path = path / "sce_sys/icon0.png";
        if (fs::exists(icon_path)) {
            if (fs::file_size(icon_path) > 4 * 1024 * 1024)
                return Error::BROKEN;
            Common::FS::IOFile input(icon_path, Common::FS::FileAccessMode::Read);
            icon.resize(input.GetSize());
            if (input.ReadRaw<u8>(icon.data(), icon.size()) != icon.size())
                return Error::INTERNAL;
        }
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
}
GuestStorage::Error GuestStorage::RestoreBackup(int uid, std::string_view tid,
                                                std::string_view directory) {
    std::lock_guard lock(mutex);
    auto status = CheckIdentity(uid, tid, directory);
    if (status != Error::OK)
        return status;
    try {
        const auto path = home / std::to_string(user) / "savedata" / title / directory;
        const auto backup = path / "sce_backup";
        if (!fs::exists(backup))
            return Error::NOT_FOUND;
        (void)Used(backup);
        PSF psf;
        if (!psf.Open(Save::GetParamSFOPath(backup)))
            return Error::BROKEN;
        const auto pending = path.parent_path() / (".restore-" + std::string(directory));
        fs::remove_all(pending);
        CopyTree(backup, pending);
        // Keep a known-good backup in the restored version as well.
        CopyTree(backup, pending / "sce_backup");
        if (cancelled)
            return Error::INTERNAL;
        PublishDirectory(pending, path);
        SyncPath(path.parent_path(), true);
        fs::remove_all(pending);
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
}
GuestStorage::Error GuestStorage::Delete(int uid, std::string_view tid,
                                         std::string_view directory) {
    std::lock_guard lock(mutex);
    auto status = CheckIdentity(uid, tid, directory);
    if (status != Error::OK)
        return status;
    try {
        const auto path = home / std::to_string(user) / "savedata" / title / directory;
        if (!fs::exists(path))
            return Error::NOT_FOUND;
        (void)Used(path);
        const auto retired = path.parent_path() / (".delete-" + std::string(directory));
        fs::remove_all(retired);
        fs::rename(path, retired);
        SyncPath(path.parent_path(), true);
        fs::remove_all(retired);
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
}
GuestStorage::Error GuestStorage::GetEvent(Event& event) {
    std::lock_guard lock(mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (events.empty())
        return Error::NOT_FOUND;
    event = events.front();
    events.pop_front();
    return Error::OK;
}

} // namespace Core::HostRuntime
