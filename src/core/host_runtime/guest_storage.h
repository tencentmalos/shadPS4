// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include "core/libraries/save_data/save_instance.h"
#include "core/libraries/save_data/savedata.h"
#include "core/libraries/save_data/savedata_error.h"

namespace Core::HostRuntime {
// Session-owned save mounts and guest descriptors. All guest pointers are copied
// or pinned by the adapter; this boundary only receives native values/spans.
class GuestStorage {
public:
    using Error = Libraries::SaveData::Error;
    struct MountResult {
        std::array<char, 16> point{};
        u64 required_blocks{};
        u32 unused{}, status{};
        std::array<u8, 32> reserved{};
    };
    struct MountInfo {
        u64 blocks{}, free_blocks{};
        std::array<u8, 32> reserved{};
    };
    struct IoResult {
        s64 value{};
        int error{};
    }; // host errno, converted by guest adapter
    GuestStorage(Core::FileSys::MntPoints& mounts, std::filesystem::path home, std::string title,
                 int user);
    ~GuestStorage();
    Error Initialize();
    Error Terminate();
    Error Mount(int user, std::string_view title, std::string_view directory, u64 blocks, u32 mode,
                MountResult& result);
    Error Unmount(std::string_view point);
    Error UnmountBackup(std::string_view point);
    Error CheckBackup(int user, std::string_view title, std::string_view directory,
                      Libraries::SaveData::OrbisSaveDataParam& param, std::vector<u8>& icon);
    Error RestoreBackup(int user, std::string_view title, std::string_view directory);
    Error Delete(int user, std::string_view title, std::string_view directory);
    struct Event {
        u32 type{}, error{};
        s32 user{};
        u32 pad{};
        Libraries::SaveData::OrbisSaveDataTitleId title{};
        Libraries::SaveData::OrbisSaveDataDirName directory{};
        std::array<u8, 40> reserved{};
    };
    Error GetEvent(Event& event);
    void Cancel() {
        cancelled = true;
    }

    Error Info(std::string_view point, MountInfo& result);
    Error GetParam(std::string_view point, u32 type, std::span<u8> out, u64& size);
    Error SetParam(std::string_view point, u32 type, std::span<const u8> in);
    IoResult Open(std::string_view path, u32 flags, u32 mode);
    IoResult Close(int fd);
    IoResult Read(int fd, std::span<u8> data);
    IoResult Write(int fd, std::span<const u8> data);
    IoResult Seek(int fd, s64 offset, int whence);
    IoResult Sync(int fd);
    IoResult Mkdir(std::string_view path, u32 mode);
    IoResult Unlink(std::string_view path);
    IoResult Rename(std::string_view from, std::string_view to);
    static bool ValidTitle(std::string_view title);

private:
    using Save = Libraries::SaveData::SaveInstance;
    struct File {
        int host{};
        int slot{-1};
        bool writable{};
        bool append{};
    };
    struct Parent {
        int fd{-1};
        int slot{-1};
        std::string leaf;
        int error{};
    };
    std::mutex mutex;
    Core::FileSys::MntPoints& mounts;
    std::filesystem::path home;
    std::string title;
    int user;
    bool initialized{};
    std::atomic<bool> cancelled{};
    std::deque<Event> events;
    void CopyTree(const std::filesystem::path& from, const std::filesystem::path& to);
    Error CheckIdentity(int uid, std::string_view tid, std::string_view directory);
    std::array<std::unique_ptr<Save>, 16> slots;
    std::map<int, File> files;
    int next_fd{3};
    Save* Find(std::string_view point);
    Parent Resolve(std::string_view path, bool write);
    Error UnmountLocked(std::string_view point);
    static u64 Used(const std::filesystem::path& root);
};
static_assert(sizeof(GuestStorage::MountResult) == 64);
static_assert(sizeof(GuestStorage::MountInfo) == 48);
} // namespace Core::HostRuntime
