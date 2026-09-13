// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <future>
#include "core/guest_cpu/api/access_fault.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
static void CheckGpuContracts() {
    using Core::GuestCpu::AccessFaultHandler;
    CHECK(!AccessFaultHandler::Dispatch(nullptr, reinterpret_cast<void*>(0x1234)));
    struct State {
        std::atomic<bool> entered{}, release{}, retired{};
    } state;
    auto handler =
        std::make_unique<AccessFaultHandler>(&state, [](void* owner, void*, void* address) {
            if (address != reinterpret_cast<void*>(0x1234))
                return false;
            auto& state = *static_cast<State*>(owner);
            state.entered = true;
            state.entered.notify_all();
            while (!state.release)
                state.release.wait(false);
            return true;
        });
    CHECK(!AccessFaultHandler::Dispatch(nullptr, nullptr));
    bool duplicate_refused{};
    try {
        AccessFaultHandler duplicate(nullptr, nullptr);
    } catch (const std::logic_error&) {
        duplicate_refused = true;
    }
    CHECK(duplicate_refused);
    auto reader = std::async(std::launch::async, [] {
        return AccessFaultHandler::Dispatch(nullptr, reinterpret_cast<void*>(0x1234));
    });
    while (!state.entered)
        state.entered.wait(false);
    std::promise<void> retire_started;
    auto retire = std::async(std::launch::async, [&] {
        retire_started.set_value();
        handler.reset();
        state.retired = true;
    });
    retire_started.get_future().wait();
    CHECK(!state.retired);
    state.release = true;
    state.release.notify_all();
    CHECK(reader.get());
    retire.get();
    CHECK(state.retired);
    CHECK(!AccessFaultHandler::Dispatch(nullptr, reinterpret_cast<void*>(0x1234)));
    using namespace Libraries::GnmDriver;
    const std::array<u64, 3> guest_addresses{0x1200010000ULL, 0x1200020000ULL, 0x1200030000ULL};
    BindEmbeddedShaders(guest_addresses);
    std::array<u32, 64> command{};
    for (u32 id = 0; id < 2; ++id) {
        CHECK(sceGnmSetEmbeddedPsShader(command.data(), command.size(), id, 0) == 0);
        CHECK(command[2] == u32(guest_addresses[id] >> 8));
        CHECK(command[3] == 0);
        CHECK(!GetEmbeddedShader(id).empty());
    }
    CHECK(sceGnmSetEmbeddedVsShader(command.data(), command.size(), 0, 0) == 0);
    CHECK(command[2] == u32(guest_addresses[2] >> 8));
    CHECK(command[3] == 0);
    BindEmbeddedShaders({});
}
