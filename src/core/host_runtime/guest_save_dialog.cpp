// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#include <nlohmann/json.hpp>
#include "core/libraries/save_data/dialog/savedatadialog_ui.h"
#include "guest_save_dialog.h"
#include "guest_storage.h"
namespace Core::HostRuntime {
using namespace Libraries::SaveData;
using namespace Libraries::SaveData::Dialog;
using namespace GuestCpu;
namespace CD = Libraries::CommonDialog;
namespace {
template <class T>
bool Read(GuestAddressSpace& space, u64 addr, T& out) {
    return bool(space.Read(GuestAddress{addr}, std::as_writable_bytes(std::span{&out, 1})));
}
template <class T>
u64 Address(T* pointer) {
    return reinterpret_cast<u64>(pointer);
}
std::optional<std::string> String(GuestAddressSpace& space, u64 addr, size_t max) {
    if (!addr || addr > UINT64_MAX - max)
        return {};
    std::string out;
    for (size_t i = 0; i < max; ++i) {
        char c{};
        if (!Read(space, addr + i, c))
            return {};
        if (!c)
            return out;
        out += c;
    }
    return {};
}
bool Directory(std::string_view s) {
    return !s.empty() && s != "." && s != ".." && s.size() < 32 && s.find_first_of("/\\") == s.npos;
}
} // namespace
bool GuestSaveDialog::IsSaveNid(std::string_view n) {
    return std::ranges::find(SaveDialogNids, n) != std::end(SaveDialogNids);
}
bool GuestSaveDialog::IsCommonNid(std::string_view n) {
    return n == "uoUpLGNkygk" || n == "BQ3tey0JmQM";
}
void GuestSaveDialog::Configure(std::filesystem::path h, std::string t, int u) {
    std::lock_guard lock(mutex);
    home = std::filesystem::weakly_canonical(h);
    title = std::move(t);
    user = u;
}
void GuestSaveDialog::Cancel() {
    std::lock_guard lock(mutex);
    stopped = true;
    if (status == 2) {
        status = 3;
        result = 1;
        button = 0;
    }
}
std::string GuestSaveDialog::SnapshotJson() const {
    std::lock_guard lock(mutex);
    if (stopped || status != 2)
        return {};
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& i : items)
        rows.push_back(i.label);
    nlohmann::json j = {{"request", request}, {"mode", mode},         {"text", text},
                        {"buttons", buttons}, {"cancel", can_cancel}, {"progress", progress},
                        {"items", rows}};
    return j.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
}
bool GuestSaveDialog::Respond(u64 id, int action, int selection) {
    std::lock_guard lock(mutex);
    if (stopped || status != 2 || id != request)
        return false;
    if (action == 0) {
        if (!can_cancel)
            return false;
        result = 1;
        button = 0;
        selected = -1;
    } else if (action == 1 || action == 2) {
        if (action == 2 && buttons != 1)
            return false;
        if (buttons == 2)
            return false;
        if (mode == 1 && (selection < 0 || size_t(selection) >= items.size()))
            return false;
        result = ok_is_cancel ? 1 : 0;
        button = ok_is_cancel ? 0 : action;
        selected = mode == 1 ? selection : (items.empty() ? -1 : 0);
    } else
        return false;
    status = 3;
    return true;
}
u64 GuestSaveDialog::Invoke(std::string_view nid, GuestAddressSpace& space,
                            const std::array<u64, 6>& a) {
    std::lock_guard lock(mutex);
    auto err = [](CD::Error e) { return static_cast<u32>(e); };
    if (stopped)
        return err(CD::Error::INVALID_STATE);
    if (nid == "uoUpLGNkygk") {
        if (common)
            return err(CD::Error::ALREADY_SYSTEM_INITIALIZED);
        common = true;
        return 0;
    }
    if (nid == "BQ3tey0JmQM")
        return initialized;
    if (nid == "s9e3+YpRnzw") {
        if (!common)
            return err(CD::Error::NOT_SYSTEM_INITIALIZED);
        if (initialized)
            return err(CD::Error::ALREADY_INITIALIZED);
        initialized = true;
        status = 1;
        return 0;
    }
    if (nid == "en7gNVnh878")
        return initialized && !stopped;
    if (nid == "ERKzksauAJA" || nid == "KK3Bdg1RWK0")
        return status;
    if (nid == "YuH2FA7azqQ") {
        if (!initialized)
            return err(CD::Error::NOT_INITIALIZED);
        initialized = false;
        status = 0;
        items.clear();
        return 0;
    }
    if (nid == "fH46Lag88XY") {
        if (status != 2)
            return err(CD::Error::NOT_RUNNING);
        // Explicit guest Close completes a progress operation; UI cancellation
        // follows Respond(0), which produces USER_CANCELED instead.
        status = 3;
        result = 0;
        button = 0;
        selected = -1;
        return 0;
    }
    if (nid == "V-uEeFKARJU" || nid == "hay1CfTmLyA") {
        if (status != 2)
            return err(CD::Error::NOT_RUNNING);
        if (mode != 5)
            return err(CD::Error::NOT_SUPPORTED);
        if (a[0] != 0 || a[1] > 100)
            return err(CD::Error::PARAM_INVALID);
        progress = nid == "V-uEeFKARJU" ? std::min<u64>(100, progress + a[1]) : a[1];
        return 0;
    }
    if (nid == "yEiJ-qqr6Cg") {
        if (status != 3)
            return err(CD::Error::NOT_FINISHED);
        OrbisSaveDataDialogResult out{};
        if (!Read(space, a[0], out))
            return err(CD::Error::ARG_NULL);
        auto pin = space.AcquirePinnedSpan({GuestAddress{a[0]}, sizeof(out)}, true);
        if (!pin)
            return err(CD::Error::ARG_NULL);
        std::optional<PinnedSpan> dir, param;
        if (out.dirName) {
            auto p = space.AcquirePinnedSpan(
                {GuestAddress{Address(out.dirName)}, sizeof(*out.dirName)}, true);
            if (!p)
                return err(CD::Error::PARAM_INVALID);
            dir = std::move(p).Value();
        }
        if (out.param) {
            auto p = space.AcquirePinnedSpan({GuestAddress{Address(out.param)}, sizeof(*out.param)},
                                             true);
            if (!p)
                return err(CD::Error::PARAM_INVALID);
            param = std::move(p).Value();
        }
        auto dir_addr = out.dirName;
        auto param_addr = out.param;
        out = {};
        out.dirName = dir_addr;
        out.param = param_addr;
        out.mode = static_cast<SaveDataDialogMode>(mode);
        out.result = static_cast<CD::Result>(result);
        out.buttonId = static_cast<ButtonId>(button);
        out.userData = reinterpret_cast<void*>(user_data);
        OrbisSaveDataDirName name{};
        OrbisSaveDataParam metadata{};
        if (selected >= 0) {
            name.data.FromString(items[selected].directory);
            metadata = items[selected].param;
        }
        if (dir)
            std::memcpy(dir->WritableBytes().data(), &name, sizeof(name));
        if (param)
            std::memcpy(param->WritableBytes().data(), &metadata, sizeof(metadata));
        std::memcpy(pin.Value().WritableBytes().data(), &out, sizeof(out));
        return 0;
    }
    if (nid != "4tPhsP6FpDI")
        return err(CD::Error::NOT_SUPPORTED);
    if (status != 1 && status != 3)
        return err(CD::Error::INVALID_STATE);
    OrbisSaveDataDialogParam p{};
    SaveDialogItems list{};
    if (!Read(space, a[0], p))
        return err(CD::Error::ARG_NULL);
    if (p.size != sizeof(p) || p.baseParam.size != sizeof(p.baseParam) || u32(p.mode) < 1 ||
        u32(p.mode) > 5 || u32(p.dispType) < 1 || u32(p.dispType) > 3 ||
        !Read(space, Address(p.items), list) || list.userId != user || list.dirNameNum > 1024 ||
        u32(list.focusPos) > 6 || u32(list.itemStyle) > 2)
        return err(CD::Error::PARAM_INVALID);
    if (list.titleId) {
        auto t = String(space, Address(list.titleId), 10);
        if (!t || *t != title)
            return err(CD::Error::PARAM_INVALID);
    }
    if (p.animParam) {
        AnimationParam v{};
        if (!Read(space, Address(p.animParam), v) || u32(v.userOK) > 1 || u32(v.userCancel) > 1)
            return err(CD::Error::PARAM_INVALID);
    }
    bool cancel = true;
    OptionParam opt{};
    if (p.optionParam) {
        if (!Read(space, Address(p.optionParam), opt) || u32(opt.back) > 1)
            return err(CD::Error::PARAM_INVALID);
        cancel = opt.back == OptionBack::ENABLE;
    }
    std::vector<Item> next_items;
    if (list.dirNameNum && !list.dirName)
        return err(CD::Error::PARAM_INVALID);
    for (u32 i = 0; i < list.dirNameNum; ++i) {
        if (Address(list.dirName) > UINT64_MAX - u64(i) * 32)
            return err(CD::Error::PARAM_INVALID);
        auto dir = String(space, Address(list.dirName) + u64(i) * 32, 32);
        if (!dir || !Directory(*dir))
            return err(CD::Error::PARAM_INVALID);
        auto path = home / std::to_string(user) / "savedata" / title / *dir;
        // Only read metadata from the app's existing save directory, no symlinks.
        std::error_code ec;
        auto file = path / "sce_sys/param.sfo";
        if (!std::filesystem::exists(file, ec))
            continue;
        if (std::filesystem::weakly_canonical(file, ec) != file || ec)
            return err(CD::Error::PARAM_INVALID);
        PSF sfo;
        if (!sfo.Open(file))
            return err(CD::Error::PARAM_INVALID);
        Item item;
        item.directory = *dir;
        item.param.FromSFO(sfo);
        item.label = item.param.title.to_string() + " — " + *dir;
        next_items.push_back(std::move(item));
    }
    if (list.focusPosDirName) {
        auto f = String(space, Address(list.focusPosDirName), 32);
        if (!f || !Directory(*f))
            return err(CD::Error::PARAM_INVALID);
    }
    if (list.newItem) {
        SaveDialogNewItem item{};
        if (!Read(space, Address(list.newItem), item) || item.iconSize > 4 * 1024 * 1024)
            return err(CD::Error::PARAM_INVALID);
        if (item.iconSize &&
            !space.ValidateRange({GuestAddress{Address(item.iconBuf)}, item.iconSize},
                                 GuestPermission::Read))
            return err(CD::Error::PARAM_INVALID);
        auto label = item.title ? String(space, Address(item.title), 1024)
                                : std::optional<std::string>{"New save"};
        if (!label)
            return err(CD::Error::PARAM_INVALID);
        if (p.dispType == DialogType::SAVE)
            next_items.insert(next_items.begin(), Item{"", *label, {}});
    }
    std::string message;
    u32 next_buttons = 0;
    bool cancel_result = false;
    switch (p.mode) {
    case SaveDataDialogMode::LIST:
        message = "Select saved data";
        break;
    case SaveDataDialogMode::USER_MSG: {
        UserMessageParam v{};
        if (!Read(space, Address(p.userMsgParam), v) || u32(v.buttonType) > 3 || u32(v.msgType) > 1)
            return err(CD::Error::PARAM_INVALID);
        auto msg = String(space, Address(v.msg), 4096);
        if (!msg)
            return err(CD::Error::PARAM_INVALID);
        message = *msg;
        next_buttons = u32(v.buttonType);
        cancel = cancel && (next_buttons == 3);
        break;
    }
    case SaveDataDialogMode::SYSTEM_MSG: {
        SystemMessageParam v{};
        if (!Read(space, Address(p.sysMsgParam), v))
            return err(CD::Error::PARAM_INVALID);
        switch (v.msgType) {
        case SystemMessageType::NODATA:
            message = "There is no saved data.";
            cancel_result = true;
            break;
        case SystemMessageType::CONFIRM:
            message = p.dispType == DialogType::SAVE   ? "Save this data?"
                      : p.dispType == DialogType::LOAD ? "Load this saved data?"
                                                       : "Delete this saved data?";
            next_buttons = 1;
            break;
        case SystemMessageType::OVERWRITE:
            message = "Overwrite the existing saved data?";
            next_buttons = 1;
            break;
        case SystemMessageType::NOSPACE:
        case SystemMessageType::NOSPACE_CONTINUABLE:
            message = "Insufficient storage. Required free save blocks: " + std::to_string(v.value);
            cancel_result = true;
            break;
        case SystemMessageType::PROGRESS:
            message = p.dispType == DialogType::SAVE   ? "Saving…"
                      : p.dispType == DialogType::LOAD ? "Loading…"
                                                       : "Deleting…";
            next_buttons = 2;
            cancel = p.optionParam && cancel;
            break;
        case SystemMessageType::FILE_CORRUPTED:
            message = "The saved data is corrupted.";
            cancel_result = true;
            break;
        case SystemMessageType::FINISHED:
            message = "The save operation has completed.";
            cancel_result = true;
            break;
        default:
            return err(CD::Error::NOT_SUPPORTED);
        }
        break;
    }
    case SaveDataDialogMode::ERROR_CODE: {
        ErrorCodeParam v{};
        if (!Read(space, Address(p.errorCodeParam), v))
            return err(CD::Error::PARAM_INVALID);
        message = fmt::format("Save error: {:#010x}", v.errorCode);
        break;
    }
    case SaveDataDialogMode::PROGRESS_BAR: {
        ProgressBarParam v{};
        if (!Read(space, Address(p.progressBarParam), v) || u32(v.barType) != 0 ||
            u32(v.sysMsgType) > 2)
            return err(CD::Error::PARAM_INVALID);
        if (v.msg) {
            auto msg = String(space, Address(v.msg), 4096);
            if (!msg)
                return err(CD::Error::PARAM_INVALID);
            message = *msg;
        } else
            message = v.sysMsgType == ProgressSystemMessageType::RESTORE ? "Restoring saved data…"
                                                                         : "Saving…";
        next_buttons = 2;
        cancel = p.optionParam && cancel;
        break;
    }
    default:
        return err(CD::Error::NOT_SUPPORTED);
    }
    items = std::move(next_items);
    text = std::move(message);
    buttons = next_buttons;
    mode = u32(p.mode);
    user_data = Address(p.userData);
    can_cancel = cancel;
    ok_is_cancel = cancel_result;
    progress = 0;
    selected = -1;
    result = 0;
    button = 0;
    ++request;
    status = 2;
    return 0;
}
} // namespace Core::HostRuntime
