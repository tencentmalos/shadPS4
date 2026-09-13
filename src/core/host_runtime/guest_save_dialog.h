// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/save_data/savedata.h"
namespace Core::HostRuntime {
// Platform-neutral, copied dialog model. Android polls it and submits an explicit
// request-id-tagged action; no JNI/native worker calls Java, no guest pointers
// survive Open. The host retains userData only as an opaque guest integer.
class GuestSaveDialog {
public:
    void Configure(std::filesystem::path home, std::string title, int user);
    u64 Invoke(std::string_view nid, GuestCpu::GuestAddressSpace& space,
               const std::array<u64, 6>& args);
    std::string SnapshotJson() const;
    bool Respond(u64 request, int action, int selection); // 0 cancel, 1 OK/yes, 2 no
    void Cancel();
    static bool IsSaveNid(std::string_view nid);
    static bool IsCommonNid(std::string_view nid);

private:
    mutable std::mutex mutex;
    bool common{}, initialized{}, stopped{};
    u32 status{}, mode{}, buttons{}, progress{};
    bool can_cancel{}, ok_is_cancel{};
    u64 request{}, user_data{};
    int user{};
    std::filesystem::path home;
    std::string title, text;
    struct Item {
        std::string directory, label;
        Libraries::SaveData::OrbisSaveDataParam param{};
    };
    std::vector<Item> items;
    u32 result{}, button{};
    int selected{-1};
};
inline constexpr std::string_view SaveDialogNids[]{
    "fH46Lag88XY", "yEiJ-qqr6Cg", "ERKzksauAJA", "s9e3+YpRnzw", "en7gNVnh878",
    "4tPhsP6FpDI", "V-uEeFKARJU", "hay1CfTmLyA", "YuH2FA7azqQ", "KK3Bdg1RWK0"};
} // namespace Core::HostRuntime
