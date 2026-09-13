// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <stdexcept>
#include <thread>

namespace Core::GuestCpu {
// One process signal dispatcher, owned by the exclusive native renderer session.
// The callback must refuse addresses/accesses it does not own. Unpublishing waits
// for active signal readers before the renderer or guest mappings are destroyed.
class AccessFaultHandler {
public:
    using Callback = bool (*)(void*, void*, void*);
    AccessFaultHandler(void* owner, Callback callback) : owner(owner), callback(callback) {
        if (!callback) throw std::invalid_argument("access fault callback is empty");
        AccessFaultHandler* expected{};
        if (!active.compare_exchange_strong(expected, this))
            throw std::logic_error("access fault handler already installed");
    }
    ~AccessFaultHandler() {
        auto* expected = this;
        active.compare_exchange_strong(expected, nullptr);
        while (readers.load())
            std::this_thread::yield();
    }
    AccessFaultHandler(const AccessFaultHandler&) = delete;
    static bool Dispatch(void* context, void* address) {
        readers.fetch_add(1);
        auto* handler = active.load();
        bool handled = handler && handler->callback(handler->owner, context, address);
        readers.fetch_sub(1);
        return handled;
    }

private:
    void* owner;
    Callback callback;
    static_assert(std::atomic<AccessFaultHandler*>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static std::atomic<AccessFaultHandler*> active;
    static std::atomic<unsigned> readers;
};
} // namespace Core::GuestCpu
