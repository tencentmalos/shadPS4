// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <fcntl.h>
#include <unistd.h>
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_storage_hle.h"

static void CheckGuestStorage(const std::filesystem::path& root) {
    using Core::HostRuntime::GuestStorage;
    using E = GuestStorage::Error;
    Core::FileSys::MntPoints mounts;
    const auto home = root / "save-contract";
    const auto alias = root / "save-contract-alias";
    std::filesystem::create_directories(home);
    std::filesystem::create_directory_symlink(home, alias);
    GuestStorage::MountResult result{};
    {
        PSF valid;
        valid.AddString("TITLE_ID", "CUSA99991");
        valid.AddInteger("ATTRIBUTE", 1);
        const auto encoded = valid.Encode();
        PSF parsed;
        CHECK(parsed.Open(encoded));
        for (size_t length = 0; length < encoded.size(); ++length) {
            std::vector<u8> truncated(encoded.begin(), encoded.begin() + length);
            CHECK(!parsed.Open(truncated));
        }
        auto invalid = encoded;
        invalid[22] = 0xff;
        invalid[23] = 0xff; // unknown entry format, not a host assert
        CHECK(!parsed.Open(invalid));
        CHECK(parsed.GetString("TITLE_ID").value_or("") == "CUSA99991");
    }
    const std::array<u8, 8> value{1, 3, 5, 7, 9, 11, 13, 15};
    {
        GuestStorage storage(mounts, alias, "CUSA99991", 1000);
        CHECK(storage.Mount(1000, "", "slot", 96, 34, result) == E::NOT_INITIALIZED);
        CHECK(storage.Initialize() == E::OK);
        CHECK(storage.Mount(1001, "", "slot", 96, 34, result) == E::INVALID_LOGIN_USER);
        CHECK(storage.Mount(1000, "CUSA99992", "slot", 96, 34, result) == E::PARAMETER);
        CHECK(storage.Mount(1000, "", "../escape", 96, 34, result) == E::PARAMETER);
        CHECK(storage.Mount(1000, "", "slot", 96, 38, result) == E::PARAMETER);
        CHECK(storage.Mount(1000, "", "slot", 96, 1, result) == E::NOT_FOUND);
        CHECK(storage.Mount(1000, "", "slot", 95, 34, result) == E::PARAMETER);
        CHECK(storage.Mount(1000, "", "slot", 96, 34, result) == E::OK && result.status == 1);
        CHECK(std::string(result.point.data()) == "/savedata0");
        CHECK(storage.Mount(1000, "", "slot", 96, 34, result) == E::BUSY);
        CHECK(storage.Terminate() == E::BUSY);
        CHECK(storage.Open("/savedata0/../escape", 0x202, 0600).error == EACCES);
        CHECK(storage.Open("/savedata0/sce_sys/param.sfo", 0x402, 0600).error == EACCES);
        auto fd = storage.Open("/savedata0/value", 0x602, 0600);
        CHECK(!fd.error && fd.value >= 3);
        CHECK(storage.Unmount("/savedata0") == E::BUSY);
        CHECK(storage.Write(fd.value, value).value == 8);
        CHECK(storage.Sync(fd.value).value == 0);
        CHECK(storage.Seek(fd.value, 0, 0).value == 0);
        std::array<u8, 8> out{};
        CHECK(storage.Read(fd.value, out).value == 8 && out == value);
        CHECK(storage.Seek(fd.value, 96 * 32768, 0).value == 96 * 32768);
        CHECK(storage.Write(fd.value, value).error == ENOSPC);
        CHECK(storage.Close(fd.value).value == 0);
        CHECK(storage.Read(fd.value, out).error == EBADF);
        Libraries::SaveData::OrbisSaveDataParam param{};
        param.title.FromString("persistent title");
        param.userParam = 0x12345678;
        CHECK(storage.SetParam("/savedata0", 0,
                               {reinterpret_cast<const u8*>(&param), sizeof(param)}) == E::OK);
        CHECK(storage.Unmount("/savedata0") == E::OK);
        CHECK(storage.Open("/savedata0/value", 0, 0).error == ENOENT);
        CHECK(storage.Terminate() == E::OK);
    }
    const auto save = home / "1000/savedata/CUSA99991/slot";
    CHECK(!std::filesystem::exists(save / "sce_sys/corrupted"));
    const auto metadata_time = std::filesystem::last_write_time(save / "sce_sys/param.sfo");
    {
        GuestStorage storage(mounts, home, "CUSA99991", 1000);
        CHECK(storage.Initialize() == E::OK);
        CHECK(storage.Mount(1000, "", "slot", 96, 1, result) == E::OK);
        CHECK(storage.Open("/savedata0/value", 2, 0).error == EROFS);
        auto fd = storage.Open("/savedata0/value", 0, 0);
        std::array<u8, 8> out{};
        CHECK(storage.Read(fd.value, out).value == 8 && out == value);
        CHECK(storage.Write(fd.value, value).error == EBADF);
        CHECK(storage.Close(fd.value).value == 0);
        Libraries::SaveData::OrbisSaveDataParam param{};
        u64 size{};
        CHECK(storage.GetParam("/savedata0", 0, {reinterpret_cast<u8*>(&param), sizeof(param)},
                               size) == E::OK);
        CHECK(param.title.to_string() == "persistent title" && param.userParam == 0x12345678 &&
              size == sizeof(param));
        CHECK(
            storage.SetParam("/savedata0", 4, {reinterpret_cast<const u8*>(&param.userParam), 4}) ==
            E::BAD_MOUNTED);
        CHECK(storage.Unmount("/savedata0") == E::OK);
        CHECK(std::filesystem::last_write_time(save / "sce_sys/param.sfo") == metadata_time);
        CHECK(storage.Mount(1000, "", "slot", 96, 34, result) == E::OK && result.status == 0);
        CHECK(storage.Mkdir("/savedata0/nested", 0700).value == 0);
        CHECK(storage.Rename("/savedata0/value", "/savedata0/nested/value").value == 0);
        CHECK(storage.Rename("/savedata0/nested/value", "/savedata0/value").value == 0);
        std::filesystem::create_symlink(home, save / "escape");
        CHECK(storage.Open("/savedata0/escape/host", 0x202, 0600).error != 0);
        std::filesystem::remove(save / "escape");
        // Teardown closes outstanding descriptors and persists an active mount.
        auto active = storage.Open("/savedata0/stop", 0x202, 0600);
        CHECK(storage.Write(active.value, value).value == 8);
    }
    CHECK(!mounts.GetMount("/savedata0"));
    CHECK(!std::filesystem::exists(save / "sce_sys/corrupted"));
    {
        GuestStorage other(mounts, home, "CUSA99992", 1000);
        CHECK(other.Initialize() == E::OK);
        CHECK(other.Mount(1000, "", "slot", 96, 1, result) == E::NOT_FOUND);
    }
    {
        GuestStorage storage(mounts, home, "CUSA99991", 1000);
        CHECK(storage.Initialize() == E::OK);
        CHECK(storage.Mount(1000, "", "slot", 96, 2, result) == E::OK);
        // Actual metadata I/O failure: a directory cannot be serialized as a file.
        std::filesystem::create_directory(save / "sce_sys/param.sfo.pending");
        CHECK(storage.Unmount("/savedata0") == E::INTERNAL);
        CHECK(mounts.GetMount("/savedata0") != nullptr);
        CHECK(std::filesystem::exists(save / "sce_sys/corrupted"));
        std::filesystem::remove(save / "sce_sys/param.sfo.pending");
        CHECK(storage.Unmount("/savedata0") == E::OK);
    }
    {
        GuestStorage storage(mounts, home, "CUSA99991", 1000);
        CHECK(storage.Initialize() == E::OK);
        GuestStorage::Event event{};
        CHECK(storage.GetEvent(event) == E::NOT_FOUND);
        CHECK(storage.Mount(1000, "", "slot", 96, 2, result) == E::OK);
        CHECK(storage.UnmountBackup("/savedata0") == E::OK);
        CHECK(storage.GetEvent(event) == E::OK && event.type == 1 && event.error == 0 &&
              event.directory.data.to_string() == "slot");
        CHECK(storage.GetEvent(event) == E::NOT_FOUND);
        Libraries::SaveData::OrbisSaveDataParam param{};
        std::vector<u8> icon;
        CHECK(storage.CheckBackup(1000, "", "slot", param, icon) == E::OK);
        CHECK(param.userParam == 0x12345678);
        // Restore atomically replaces damaged live data with the committed snapshot.
        {
            std::ofstream damaged(save / "value", std::ios::binary | std::ios::trunc);
            damaged << "broken";
        }
        CHECK(storage.RestoreBackup(1000, "", "slot") == E::OK);
        CHECK(storage.Mount(1000, "", "slot", 96, 1, result) == E::OK);
        auto fd = storage.Open("/savedata0/value", 0, 0);
        std::array<u8, 8> restored{};
        CHECK(storage.Read(fd.value, restored).value == 8 && restored == value);
        CHECK(storage.Close(fd.value).value == 0);
        CHECK(storage.Delete(1000, "", "slot") == E::BUSY);
        CHECK(storage.Unmount("/savedata0") == E::OK);
        CHECK(storage.Mount(1000, "", "slot", 96, 2, result) == E::OK);
        CHECK(storage.UnmountBackup("/savedata0") == E::OK); // replaces an existing backup
        CHECK(storage.GetEvent(event) == E::OK && !event.error);
        storage.Cancel();
        CHECK(storage.Mount(1000, "", "slot", 96, 2, result) == E::OK);
        CHECK(storage.UnmountBackup("/savedata0") == E::INTERNAL);
        CHECK(storage.GetEvent(event) == E::OK && event.error == u32(E::INTERNAL));
        CHECK(storage.CheckBackup(1000, "", "slot", param, icon) ==
              E::OK); // previous backup survives
        CHECK(storage.Delete(1000, "", "slot") == E::OK);
        CHECK(storage.CheckBackup(1000, "", "slot", param, icon) == E::NOT_FOUND);
    }
    // Exact test-owned root only; never use production title data as a fixture.
    std::filesystem::remove(alias);
    std::filesystem::remove_all(home);
}

#include "core/host_runtime/guest_save_dialog.h"
#include "core/libraries/save_data/dialog/savedatadialog_ui.h"
static void CheckSaveDialog(const std::filesystem::path& root) {
    using namespace Core::GuestCpu;
    using namespace Core::HostRuntime;
    using namespace Libraries::SaveData::Dialog;
    using CE = Libraries::CommonDialog::Error;
    AddressSpaceConfig config{};
    config.reservation_size = 65536;
    auto made = GuestAddressSpace::Create(config);
    CHECK(bool(made));
    if (!made)
        return;
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    CHECK(bool(
        space->Map({GuestAddress{base}, 32768}, GuestPermission::Read | GuestPermission::Write)));
    auto put = [&](u64 offset, const auto& value) {
        CHECK(bool(space->Write(GuestAddress{base + offset}, std::as_bytes(std::span{&value, 1}))));
    };
    for (int round = 0; round < 3; ++round) {
        GuestSaveDialog dialog;
        dialog.Configure(root, "CUSA99991", 1000);
        auto call = [&](std::string_view nid, std::array<u64, 6> a = std::array<u64, 6>{}) {
            return dialog.Invoke(nid, *space, a);
        };
        CHECK(call("s9e3+YpRnzw") == u32(CE::NOT_SYSTEM_INITIALIZED));
        CHECK(call("uoUpLGNkygk") == 0);
        CHECK(call("s9e3+YpRnzw") == 0);
        CHECK(call("s9e3+YpRnzw") == u32(CE::ALREADY_INITIALIZED));
        OrbisSaveDataDialogParam p{};
        p.size = sizeof(p);
        p.baseParam.size = sizeof(p.baseParam);
        p.mode = SaveDataDialogMode::USER_MSG;
        p.dispType = DialogType::SAVE;
        p.items = reinterpret_cast<SaveDialogItems*>(base + 4096);
        p.userMsgParam = reinterpret_cast<UserMessageParam*>(base + 8192);
        p.userData = reinterpret_cast<void*>(0x12345678);
        SaveDialogItems items{};
        items.userId = 1000;
        UserMessageParam msg{};
        msg.buttonType = ButtonType::OKCANCEL;
        msg.msg = reinterpret_cast<const char*>(base + 12288);
        const std::array<char, 6> text{'h', 'e', 'l', 'l', 'o', 0};
        put(0, p);
        put(4096, items);
        put(8192, msg);
        put(12288, text);
        CHECK(call("4tPhsP6FpDI", {1}) == u32(CE::ARG_NULL));
        CHECK(dialog.SnapshotJson().empty());
        CHECK(call("4tPhsP6FpDI", {base}) == 0);
        CHECK(call("ERKzksauAJA") == 2);
        CHECK(dialog.SnapshotJson().find("hello") != std::string::npos);
        CHECK(call("yEiJ-qqr6Cg", {base + 16384}) == u32(CE::NOT_FINISHED));
        CHECK(!dialog.Respond(2, 0, -1));
        CHECK(!dialog.Respond(1, 2, -1));
        CHECK(dialog.Respond(1, 0, -1));
        CHECK(!dialog.Respond(1, 0, -1));
        CHECK(call("ERKzksauAJA") == 3);
        OrbisSaveDataDialogResult out{};
        out.dirName = reinterpret_cast<Libraries::SaveData::OrbisSaveDataDirName*>(1);
        put(16384, out);
        CHECK(call("yEiJ-qqr6Cg", {base + 16384}) == u32(CE::PARAM_INVALID));
        out = {};
        put(16384, out);
        CHECK(call("yEiJ-qqr6Cg", {base + 16384}) == 0);
        CHECK(bool(
            space->Read(GuestAddress{base + 16384}, std::as_writable_bytes(std::span{&out, 1}))));
        CHECK(out.result == Libraries::CommonDialog::Result::USER_CANCELED &&
              out.userData == p.userData);
        CHECK(call("4tPhsP6FpDI", {base}) == 0);
        CHECK(!dialog.Respond(1, 1, -1));
        CHECK(dialog.Respond(2, 1, -1));
        CHECK(call("yEiJ-qqr6Cg", {base + 16384}) == 0);
        CHECK(bool(
            space->Read(GuestAddress{base + 16384}, std::as_writable_bytes(std::span{&out, 1}))));
        CHECK(out.result == Libraries::CommonDialog::Result::OK && out.buttonId == ButtonId::OK);
        CHECK(call("4tPhsP6FpDI", {base}) == 0);
        dialog.Cancel();
        CHECK(dialog.SnapshotJson().empty());
        CHECK(!dialog.Respond(3, 1, -1));
    }
}
