// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include "core/guest_cpu/hle/call_adapter.h"
namespace Core::HostRuntime {
class GuestGraphics;
void InstallGraphicsHandlers(
    std::map<std::string, std::function<GuestCpu::Status(GuestCpu::Hle::HleCallFrame&)>>& handlers,
    std::set<std::string>& gnm, std::set<std::string>& video, GuestCpu::GuestAddressSpace& space,
    std::function<GuestGraphics&()> graphics);
} // namespace Core::HostRuntime
