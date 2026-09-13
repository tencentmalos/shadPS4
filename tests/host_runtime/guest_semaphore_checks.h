// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <future>
#include "core/host_runtime/guest_semaphore.h"
static void CheckGuestSemaphores() {
    using namespace Core::GuestCpu;
    using Core::HostRuntime::GuestSemaphoreDomain;
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
    u64 next = base + 4096;
    GuestSemaphoreDomain sem(*space, [&] { return next += 64; });
    CHECK(sem.Init(0, 0, 0) == POSIX_EFAULT);
    CHECK(sem.Init(base, 1, 0) == POSIX_EINVAL);
    CHECK(sem.Init(base, 0, UINT32_MAX) == POSIX_EINVAL);
    CHECK(sem.Init(base, 0, 0) == 0);
    CHECK(sem.Init(base, 0, 0) == POSIX_EBUSY);
    CHECK(sem.Wait(base, true, {}) == POSIX_EAGAIN);
    CHECK(sem.Post(base) == 0);
    CHECK(sem.Wait(base, false, {}) == 0);
    CHECK(sem.Wait(base, false, {}, std::chrono::steady_clock::now()) == POSIX_ETIMEDOUT);
    CHECK(sem.GetValue(base, 0) == POSIX_EFAULT);
    CHECK(sem.GetValue(base, base + 32) == 0);
    CHECK(sem.Init(base + 8, 0, GuestSemaphoreDomain::MaxValue) == 0);
    CHECK(sem.Post(base + 8) == POSIX_EOVERFLOW);
    std::stop_source cancel;
    std::promise<void> started;
    auto waiter = std::async(std::launch::async, [&] {
        started.set_value();
        return sem.Wait(base, false, cancel.get_token());
    });
    started.get_future().wait();
    // Cancellation must work both before and after waiter admission.
    CHECK(sem.GetValue(base, base + 32) == 0);
    cancel.request_stop();
    CHECK(waiter.get() == POSIX_EINTR);
    CHECK(sem.Destroy(base) == 0);
    CHECK(sem.Post(base) == POSIX_EINVAL);
    CHECK(sem.Destroy(base) == POSIX_EINVAL);
    CHECK(sem.Destroy(base + 8) == 0);
}
