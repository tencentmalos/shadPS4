// SPDX-License-Identifier: GPL-2.0-or-later
#include <bit>
#include <cstring>
#include <tuple>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "guest_graphics.h"
#include "guest_graphics_hle.h"
#include "video_core/texture_cache/image_info.h"
namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace GuestCpu::Hle;
using namespace Libraries;
namespace {
template <class T>
T Must(Result<T> value) {
    if (!value)
        throw std::runtime_error(Describe(value.GetError()));
    return std::move(value).Value();
}
template <class T>
T Read(GuestAddressSpace& space, u64 address) {
    T value{};
    auto status = space.Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1}));
    if (!status)
        throw std::runtime_error("invalid graphics input address");
    return value;
}
template <class T>
auto Output(GuestAddressSpace& space, u64 address) {
    return Must(space.AcquirePinnedSpan({GuestAddress{address}, sizeof(T)}, true));
}
using Args = std::array<u64, 10>;
// Explicit policies below convert every native pointer. No pointer inferred from
// the imported symbol descriptor is ever admitted.
template <class R, class... A>
struct Signature {
    using Types = std::tuple<A...>;
    static constexpr size_t count = sizeof...(A);
};
template <class R, class... A>
Signature<R, A...> Describe(R (*)(A...));
template <auto Fn, size_t... I>
u64 EncoderCall(u32* output, const Args& a, const u32* regs, const char* marker,
                std::index_sequence<I...>) {
    using S = decltype(Describe(Fn));
    auto argument = [&]<size_t Index>() {
        using T = std::tuple_element_t<Index, typename S::Types>;
        if constexpr (Index == 0)
            return output;
        else if constexpr (std::is_same_v<T, const u32*>)
            return regs;
        else if constexpr (std::is_same_v<T, const char*>)
            return marker;
        else {
            static_assert(!std::is_pointer_v<T>);
            return static_cast<T>(a[Index]);
        }
    };
    return static_cast<u32>(Fn(argument.template operator()<I>()...));
}
} // namespace
void InstallGraphicsHandlers(std::map<std::string, std::function<Status(HleCallFrame&)>>& handlers,
                             std::set<std::string>& gnm, std::set<std::string>& video,
                             GuestAddressSpace& space, std::function<GuestGraphics&()> graphics) {
    auto compute = std::make_shared<std::map<u32, std::array<u64, 3>>>();
    auto install = [&](std::string nid, unsigned argc, auto fn, bool is_video = false) {
        (is_video ? video : gnm).insert(nid);
        const bool native_memory = nid != "fzyMKs9kim0" && nid != "j6RaAUlaLv0";
        handlers[nid] = [&, graphics, argc, fn, native_memory](HleCallFrame& frame) -> Status {
            CallCursor cursor(frame);
            Args args{};
            for (unsigned i = 0; i < argc; ++i) {
                auto value = cursor.NextInteger();
                if (!value)
                    return value.GetError();
                args[i] = value.Value();
            }
            auto& renderer = graphics();
            // Native HLE is outside FEX's execution lease. Keep GPU publication
            // admitted through enqueue, so VM quiescence cannot observe an idle
            // GPU and then race a delayed submission with backing replacement.
            std::optional<ExecutionLease> admission;
            if (native_memory) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                for (;;) {
                    auto acquired = space.AcquireExecutionLease();
                    if (acquired) { admission.emplace(std::move(acquired).Value()); break; }
                    auto* scope = HleScope::Current();
                    if (acquired.GetError().category != ErrorCategory::Busy || !scope ||
                        scope->CancellationToken().stop_requested() || std::chrono::steady_clock::now() >= deadline)
                        return acquired.GetError();
                    scope->WaitFor(std::chrono::milliseconds(1));
                }
            }
            frame.registers.Set(Gpr::Rax, fn(renderer, args));
            return Ok();
        };
    };
    auto encoder = [&]<auto Fn>(const char* nid, size_t register_words = 0, bool marker = false) {
        using S = decltype(Describe(Fn));
        const bool embedded_ps = std::string_view(nid) == "X9Omw9dwv5M";
        install(nid, S::count,
                [&, register_words, marker, embedded_ps](GuestGraphics&, const Args& a) -> u64 {
                    if (!a[0] || !a[1] || a[1] > 0x100000 || (embedded_ps && a[1] < 40))
                        return u32(-1);
                    std::vector<u32> registers(register_words);
                    if (register_words && a[2]) {
                        auto result = space.Read(GuestAddress{a[2]},
                                                 std::as_writable_bytes(std::span{registers}));
                        if (!result)
                            return u32(-1);
                    }
                    std::string text;
                    if (marker) {
                        for (u64 i = 0; i < 1024; ++i) {
                            char ch = Read<char>(space, a[2] + i);
                            if (!ch)
                                break;
                            text += ch;
                        }
                        if (text.size() == 1024 || a[1] < (text.size() + 4) / 4 + 2)
                            return u32(-1);
                    }

                    // Validate the entire dword capacity and preserve unwritten padding.
                    auto pin = space.AcquirePinnedSpan({GuestAddress{a[0]}, a[1] * 4}, true);
                    if (!pin)
                        return u32(-1);
                    constexpr u32 guard = 0xa55a9187;
                    std::vector<u32> encoded(std::max<u64>(a[1], 4096) + 64, guard);
                    std::memcpy(encoded.data(), pin.Value().Bytes().data(), a[1] * 4);
                    const auto result = EncoderCall<Fn>(
                        encoded.data(), a, a[2] && register_words ? registers.data() : nullptr,
                        marker ? text.c_str() : nullptr, std::make_index_sequence<S::count>{});
                    if (!std::all_of(encoded.begin() + a[1], encoded.end(),
                                     [](u32 value) { return value == guard; }))
                        return u32(-1);
                    std::memcpy(pin.Value().WritableBytes().data(), encoded.data(), a[1] * 4);
                    return result;
                });
    };
#define ENCODE(nid, fn) encoder.template operator()<&GnmDriver::fn>(nid)
#define SHADER(nid, fn, words) encoder.template operator()<&GnmDriver::fn>(nid, words)
    ENCODE("ffrNQOshows", sceGnmComputeWaitOnAddress);
    ENCODE("0BzLGljcwBo", sceGnmDispatchDirect);
    ENCODE("Z43vKp5k7r0", sceGnmDispatchIndirect);
    ENCODE("wED4ZXCFJT0", sceGnmDispatchIndirectOnMec);
    ENCODE("nF6bFRUBRAU", sceGnmDispatchInitDefaultHardwareState);
    ENCODE("HlTPoZ-oY7Y", sceGnmDrawIndex);
    ENCODE("GGsn7jMTxw4", sceGnmDrawIndexAuto);
    ENCODE("ED9-Fjr8Ta4", sceGnmDrawIndexIndirect);
    ENCODE("thbPcG7E7qk", sceGnmDrawIndexIndirectCountMulti);
    ENCODE("oYM+YzfCm2Y", sceGnmDrawIndexOffset);
    ENCODE("4v+otIIdjqg", sceGnmDrawIndirect);
    ENCODE("yb2cRhagD1I", sceGnmDrawInitDefaultHardwareState350);
    ENCODE("im2ZuItabu4", sceGnmDrawInitToDefaultContextState400);
    ENCODE("NfvOrNzy6sk", sceGnmInsertDingDongMarker);
    ENCODE("7qZVNgEu+SY", sceGnmInsertPopMarker);
    encoder.template operator()<&GnmDriver::sceGnmInsertPushColorMarker>("aPIZJTXC+cU", 0, true);
    encoder.template operator()<&GnmDriver::sceGnmInsertPushMarker>("W1Etj-jlW7Y", 0, true);
    encoder.template operator()<&GnmDriver::sceGnmInsertSetMarker>("jiItzS6+22g", 0, true);
    install("1qXLHIpROPE", 4, [&](GuestGraphics& graphics, const Args& a) -> u64 {
        auto* port = graphics.VideoOut().GetPort(a[2]);
        if (!port || !port->is_open || a[3] >= VideoOut::MaxDisplayBuffers || a[1] != 7)
            return u32(-1);
        auto pin = Must(space.AcquirePinnedSpan({GuestAddress{a[0]}, a[1] * 4}, true));
        return u32(GnmDriver::sceGnmInsertWaitFlipDone(
            reinterpret_cast<u32*>(pin.WritableBytes().data()), a[1], a[2], a[3]));
    });
    ENCODE("MYRtYhojKdA", sceGnmResetVgtControl);
    ENCODE("X9Omw9dwv5M", sceGnmSetEmbeddedPsShader);
    ENCODE("+AFvOEXrKJk", sceGnmSetEmbeddedVsShader);
    ENCODE("cFCp0NX8wf0", sceGnmSetVgtControl);
    SHADER("Kx-h-nWQJ8A", sceGnmSetCsShaderWithModifier, 7);
    SHADER("FUHG8sQ3R58", sceGnmSetEsShader, 4);
    SHADER("UJwNuMBcUAk", sceGnmSetGsShader, 7);
    SHADER("VJNjFtqiF5w", sceGnmSetHsShader, 7);
    SHADER("vckdzbQ46SI", sceGnmSetLsShader, 4);
    SHADER("5uFKckiJYRM", sceGnmSetPsShader350, 12);
    SHADER("gAhCn6UiU4Y", sceGnmSetVsShader, 7);
    SHADER("nLM2i2+65hA", sceGnmUpdateGsShader, 7);
    SHADER("GNlx+y7xPdE", sceGnmUpdateHsShader, 7);
    SHADER("mLVL7N7BVBg", sceGnmUpdatePsShader350, 12);
    SHADER("V31V01UiScY", sceGnmUpdateVsShader, 7);
#undef ENCODE
#undef SHADER
    install(
        "w3BY+tAEiQY", 5,
        [&](GuestGraphics& graphics, const Args& a) -> u64 {
            using namespace VideoOut;
            auto* port = graphics.VideoOut().GetPort(s32(a[0]));
            if (!port || !port->is_open)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            const s32 start = a[1], count = a[3];
            if (start < 0 || start >= MaxDisplayBuffers || count <= 0 ||
                count > MaxDisplayBuffers - start)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE);
            auto attribute = Read<BufferAttribute>(space, a[4]);
            if (!attribute.width || !attribute.height || attribute.width > 16384 ||
                attribute.height > 16384 || attribute.pitch_in_pixel < attribute.width ||
                attribute.pitch_in_pixel > 32768 || attribute.tiling_mode < TilingMode::Tile ||
                attribute.tiling_mode > TilingMode::Linear)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE);
            switch (attribute.pixel_format) {
            case PixelFormat::A8R8G8B8Srgb:
            case PixelFormat::A8B8G8R8Srgb:
                break;
            case PixelFormat::A2R10G10B10:
            case PixelFormat::A2R10G10B10Srgb:
            case PixelFormat::A2R10G10B10Bt2020Pq:
                break;
            default:
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE);
            }
            std::array<void*, MaxDisplayBuffers> addresses{};
            for (s32 i = 0; i < count; ++i) {
                const auto address = Read<u64>(space, a[2] + i * 8);
                VideoCore::ImageInfo info(BufferAttributeGroup{true, attribute}, address);
                if (address % 256 || !space.ValidateRange({GuestAddress{address}, info.guest_size},
                                                          GuestPermission::Read))
                    return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS);
                addresses[i] = reinterpret_cast<void*>(address);
            }
            return u32(graphics.VideoOut().RegisterBuffers(port, start, addresses.data(), count,
                                                           &attribute));
        },
        true);
    install(
        "uquVH4-Du78", 1,
        [](GuestGraphics& graphics, const Args& a) -> u64 {
            if (a[0] != 1)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            graphics.WaitIdle();
            graphics.VideoOut().Close(a[0]);
            return 0;
        },
        true);
    install(
        "j6RaAUlaLv0", 1,
        [](GuestGraphics&, const Args& a) -> u64 {
            return u32(VideoOut::sceVideoOutWaitVblank(a[0]));
        },
        true);
    install("b08AgtPlHPg", 0, [](GuestGraphics&, const Args&) -> u64 {
        return GnmDriver::sceGnmAreSubmitsAllowed();
    });
    install("yvZ73uQUqrk", 0,
            [](GuestGraphics&, const Args&) -> u64 { return u32(GnmDriver::sceGnmSubmitDone()); });
    auto submit = [&](const char* nid, bool workload, bool flip) {
        install(nid, 5 + workload + (flip ? 4 : 0),
                [&, workload, flip](GuestGraphics& graphics, const Args& original) -> u64 {
                    std::scoped_lock submit_lock(graphics.SubmissionMutex());
                    Args a = original;
                    if (workload)
                        std::move(a.begin() + 1, a.end(), a.begin());
                    const u32 count = a[0];
                    if (!count || count > 64 || !a[1] || !a[2] || bool(a[3]) != bool(a[4]))
                        return 0x80d11000u;
                    std::vector<std::vector<u32>> dcbs(count), ccbs(count);
                    std::vector<u32*> dp(count), cp(count);
                    std::vector<u32> ds(count), cs(count);
                    for (u32 i = 0; i < count; ++i) {
                        ds[i] = Read<u32>(space, a[2] + i * 4);
                        cs[i] = a[4] ? Read<u32>(space, a[4] + i * 4) : 0;
                        if (!ds[i] || ds[i] > 0x3ffffc || cs[i] > 0x3ffffc || ((ds[i] | cs[i]) & 3))
                            return 0x80d11000u;
                        dcbs[i].resize(ds[i] / 4);
                        ccbs[i].resize(cs[i] / 4);
                        auto r = space.Read(GuestAddress{Read<u64>(space, a[1] + i * 8)},
                                            std::as_writable_bytes(std::span{dcbs[i]}));
                        if (!r)
                            return 0x80d11000u;
                        if (cs[i] && !space.Read(GuestAddress{Read<u64>(space, a[3] + i * 8)},
                                                 std::as_writable_bytes(std::span{ccbs[i]})))
                            return 0x80d11000u;
                        dp[i] = dcbs[i].data();
                        cp[i] = ccbs[i].data();
                    }
                    // The processor takes owned command copies. Nested GPU addresses keep
                    // their guest VA; VM mutations drain GPU work before changing backing.
                    if (flip) {
                        if (ds.back() < 256 || dcbs.back()[dcbs.back().size() - 64] != 0xc03e1000)
                            return 0x80d11000u;
                        return u32(GnmDriver::sceGnmSubmitAndFlipCommandBuffers(
                            count, dp.data(), ds.data(), a[3] ? cp.data() : nullptr,
                            a[4] ? cs.data() : nullptr, a[5], a[6], a[7], a[8]));
                    }
                    return u32(GnmDriver::sceGnmSubmitCommandBuffers(
                        count, const_cast<const u32**>(dp.data()), ds.data(),
                        a[3] ? const_cast<const u32**>(cp.data()) : nullptr,
                        a[4] ? cs.data() : nullptr));
                });
    };
    submit("zwY0YV91TTI", false, false);
    submit("jRcI8VcgTz4", true, false);
    submit("xbxNatawohc", false, true);
    submit("Ga6r7H6Y0RI", true, true);
    install("D0OdFMjp46I", 2, [&](GuestGraphics& graphics, const Args& a) -> u64 {
        auto out = Output<s64>(space, a[0]);
        std::string name;
        for (u64 i = 0; i < 33; ++i) {
            const char c = Read<char>(space, a[1] + i);
            if (!c)
                break;
            name += c;
        }
        if (name.size() > 32)
            return u32(ORBIS_KERNEL_ERROR_ENAMETOOLONG);
        const auto handle = graphics.CreateEqueue(name);
        std::memcpy(out.WritableBytes().data(), &handle, sizeof(handle));
        return 0;
    });
    install("jpFjmgAC5AE", 1, [](GuestGraphics& graphics, const Args& a) -> u64 {
        return u32(graphics.DeleteEqueue(a[0]));
    });
    install("fzyMKs9kim0", 5, [&](GuestGraphics& graphics, const Args& a) -> u64 {
        const s32 capacity = a[2];
        if (capacity <= 0 || capacity > 1024)
            return u32(ORBIS_KERNEL_ERROR_EINVAL);
        auto* queue = graphics.FindEqueue(a[0]);
        if (!queue)
            return u32(ORBIS_KERNEL_ERROR_EBADF);
        const u32 micros = a[4] ? Read<u32>(space, a[4]) : 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
        // Do not retain guest pins while waiting. Pin output immediately before
        // consuming events, so a bad pointer never loses a delivered event.
        for (;;) {
            if (graphics.Stopping() || HleScope::Current()->CancellationToken().stop_requested())
                return u32(ORBIS_KERNEL_ERROR_EINTR);
            if (graphics.FindEqueue(a[0]) != queue)
                return u32(ORBIS_KERNEL_ERROR_EBADF);
            {
                auto events = Must(space.AcquirePinnedSpan(
                    {GuestAddress{a[1]}, u64(capacity) * sizeof(Kernel::OrbisKernelEvent)}, true));
                auto result = Output<s32>(space, a[3]);
                std::vector<Kernel::OrbisKernelEvent> local(capacity);
                const auto n = queue->GetTriggeredEvents(local.data(), capacity);
                if (n > 0) {
                    std::memcpy(events.WritableBytes().data(), local.data(), n * sizeof(local[0]));
                    std::memcpy(result.WritableBytes().data(), &n, sizeof(n));
                    return 0;
                }
            }
            if (a[4] && std::chrono::steady_clock::now() >= deadline)
                return u32(ORBIS_KERNEL_ERROR_ETIMEDOUT);
            HleScope::Current()->WaitFor(std::chrono::milliseconds(1));
        }
    });
    for (const auto* nid : {"D0OdFMjp46I", "jpFjmgAC5AE", "fzyMKs9kim0"})
        gnm.erase(nid);
    install(
        "HXzjK9yI30k", 3,
        [](GuestGraphics& graphics, const Args& a) -> u64 {
            auto* queue = graphics.FindEqueue(a[0]);
            auto* port = graphics.VideoOut().GetPort(a[1]);
            if (!queue)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE);
            if (!port || !port->is_open)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            std::scoped_lock lock(port->port_mutex);
            if (std::find(port->flip_events.begin(), port->flip_events.end(), a[0]) !=
                port->flip_events.end())
                return u32(ORBIS_KERNEL_ERROR_EEXIST);
            return u32(
                VideoOut::sceVideoOutAddFlipEvent(a[0], a[1], reinterpret_cast<void*>(a[2])));
        },
        true);
    install("b0xyllnVY-I", 3, [](GuestGraphics& graphics, const Args& a) -> u64 {
        auto* queue = graphics.FindEqueue(a[0]);
        if (!queue)
            return u32(ORBIS_KERNEL_ERROR_EBADF);
        if (a[1] > 6 && a[1] != 0x40)
            return u32(ORBIS_KERNEL_ERROR_EINVAL);
        if (queue->EventExists(a[1], Kernel::OrbisKernelEvent::Filter::GraphicsCore))
            return u32(ORBIS_KERNEL_ERROR_EEXIST);
        return u32(GnmDriver::sceGnmAddEqEvent(a[0], a[1], reinterpret_cast<void*>(a[2])));
    });
    install("PVT+fuoS9gU", 2, [](GuestGraphics& graphics, const Args& a) -> u64 {
        if (a[1] > 6 && a[1] != 0x40)
            return u32(ORBIS_KERNEL_ERROR_EINVAL);
        return u32(GnmDriver::sceGnmDeleteEqEvent(a[0], a[1]));
    });
    install("UoYY0DWMC0U", 1, [&](GuestGraphics&, const Args& a) -> u64 {
        auto event = Read<Kernel::OrbisKernelEvent>(space, a[0]);
        return u32(GnmDriver::sceGnmGetEqEventType(&event));
    });
    install("5udAm+6boVg", 2, [&](GuestGraphics&, const Args& a) -> u64 {
        auto out = Output<u32>(space, a[1]);
        u32 value{};
        auto result = GnmDriver::sceGnmCreateWorkloadStream(a[0], &value);
        if (!result)
            std::memcpy(out.WritableBytes().data(), &value, sizeof(value));
        return u32(result);
    });
    install("ihxrbsoSKWc", 2, [&](GuestGraphics&, const Args& a) -> u64 {
        auto out = Output<u64>(space, a[1]);
        u64 value{};
        auto result = GnmDriver::sceGnmBeginWorkload(a[0], &value);
        if (!result)
            std::memcpy(out.WritableBytes().data(), &value, sizeof(value));
        return u32(result);
    });
    install("Fa3x75OOLRA", 1, [](GuestGraphics&, const Args& a) -> u64 {
        return u32(GnmDriver::sceGnmEndWorkload(a[0]));
    });
    install("Fwvh++m9IQI", 0, [](GuestGraphics&, const Args&) -> u64 {
        return GnmDriver::sceGnmGetGpuCoreClockFrequency();
    });
    install("TLV4mswiZ4A", 0, [](GuestGraphics&, const Args&) -> u64 {
        return GnmDriver::sceGnmDriverCaptureInProgress();
    });
    install("R6z1xM3pW-w", 0, [](GuestGraphics&, const Args&) -> u64 {
        return GnmDriver::sceGnmDriverTraceInProgress();
    });

    auto map_compute = [&](const char* nid, bool priority) {
        install(
            nid, priority ? 6 : 5,
            [&, compute, priority](GuestGraphics& graphics, const Args& a) -> u64 {
                std::scoped_lock submit_lock(graphics.SubmissionMutex());
                if (!a[3] || a[3] > 0x100000 || !std::has_single_bit(a[3]) || a[2] % 256 ||
                    a[4] % 4 ||
                    !space.ValidateRange({GuestAddress{a[2]}, a[3] * 4}, GuestPermission::Read) ||
                    !space.ValidateRange({GuestAddress{a[4]}, 4}, GuestPermission::Write))
                    return u32(-1);
                auto pin = Output<u32>(space, a[4]);
                if (compute->size() >= 56) return u32(-1);
            const auto result = GnmDriver::sceGnmMapComputeQueue(
                    a[0], a[1], a[2], a[3], reinterpret_cast<u32*>(pin.WritableBytes().data()));
                if (result > 0 && result < 64)
                    (*compute)[result] = {a[2], a[3], a[4]};
                return u32(result);
            });
    };
    map_compute("29oKvKXzEZo", false);
    map_compute("A+uGq+3KFtQ", true);
    for (const auto* nid : {"bX5IbRvECXk", "byXlqupd8cE"}) {
        install(nid, 2, [&, compute](GuestGraphics& graphics, const Args& a) -> u64 {
            std::scoped_lock submit_lock(graphics.SubmissionMutex());
            const auto it = compute->find(a[0]);
            if (it == compute->end() || a[1] >= it->second[1])
                throw std::runtime_error("invalid compute doorbell");
            if (!space.ValidateRange({GuestAddress{it->second[0]}, it->second[1] * 4},
                                     GuestPermission::Read) ||
                !space.ValidateRange({GuestAddress{it->second[2]}, 4}, GuestPermission::Write))
                throw std::runtime_error("compute queue backing retired");
            GnmDriver::sceGnmDingDong(a[0], a[1]);
            return 0;
        });
    }
    install("ArSg-TGinhk", 1, [compute](GuestGraphics& graphics, const Args& a) -> u64 {
        std::scoped_lock submit_lock(graphics.SubmissionMutex());
        if (!compute->contains(a[0]))
            return u32(-1);
        graphics.WaitIdle();
        auto result = GnmDriver::sceGnmUnmapComputeQueue(a[0]);
        if (!result)
            compute->erase(a[0]);
        return u32(result);
    });

    install(
        "OcQybQejHEY", 2,
        [&](GuestGraphics&, const Args& a) -> u64 {
            auto out = Output<u64>(space, a[1]);
            uintptr_t address{};
            const auto result = VideoOut::sceVideoOutGetBufferLabelAddress(a[0], &address);
            if (result > 0) {
                if (!space.ValidateRange({GuestAddress{address}, u64(result) * 8},
                                         GuestPermission::Read | GuestPermission::Write))
                    throw std::runtime_error("VideoOut label is not guest memory");
                std::memcpy(out.WritableBytes().data(), &address, 8);
            }
            return u32(result);
        },
        true);
    install(
        "SbU3dwp80lQ", 2,
        [&](GuestGraphics& graphics, const Args& a) -> u64 {
            auto out = Output<VideoOut::FlipStatus>(space, a[1]);
            auto* port = graphics.VideoOut().GetPort(a[0]);
            if (!port || !port->is_open)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            std::scoped_lock lock(port->port_mutex);
            std::memcpy(out.WritableBytes().data(), &port->flip_status, sizeof(port->flip_status));
            return 0;
        },
        true);
    install(
        "U46NwOiJpys", 4,
        [](GuestGraphics& graphics, const Args& a) -> u64 {
            std::scoped_lock lock(graphics.SubmissionMutex());
            return u32(VideoOut::sceVideoOutSubmitFlip(a[0], a[1], a[2], a[3]));
        },
        true);
    install(
        "CBiu4mCE1DA", 2,
        [](GuestGraphics& graphics, const Args& a) -> u64 {
            auto* port = graphics.VideoOut().GetPort(a[0]);
            if (!port || !port->is_open)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            if (a[1] > 2)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE);
            port->flip_rate = a[1];
            return 0;
        },
        true);
    install(
        "zgXifHT9ErY", 1,
        [](GuestGraphics& graphics, const Args& a) -> u64 {
            auto* port = graphics.VideoOut().GetPort(a[0]);
            if (!port || !port->is_open)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            std::scoped_lock lock(port->port_mutex);
            return port->flip_status.flip_pending_num;
        },
        true);
    install(
        "N5KDtkIjjJ4", 2,
        [](GuestGraphics& graphics, const Args& a) -> u64 {
            auto* port = graphics.VideoOut().GetPort(a[0]);
            if (!port || !port->is_open)
                return u32(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
            graphics.WaitIdle();
            return u32(graphics.VideoOut().UnregisterBuffers(port, a[1]));
        },
        true);
    install("FFVZcCu3zWU", 0, [](GuestGraphics&, const Args&) -> u64 {
        return GnmDriver::sceGnmGetOffChipTessellationBufferSize();
    });
    install("nvEwfYAImTs", 7, [](GuestGraphics&, const Args&) -> u64 {
        // Resource registration is unavailable on retail. The native routine
        // consumes none of its pointer arguments and returns the retail error.
        return u32(GnmDriver::sceGnmRegisterResource(nullptr, nullptr, nullptr, 0, nullptr, 0, 0));
    });
#define RETAIL(nid, fn) install(nid, 0, [](GuestGraphics&, const Args&) -> u64 { return u32(GnmDriver::fn()); })
    RETAIL("yhFCnaz5daw", sceGnmUnregisterAllResourcesForOwner);
    RETAIL("fhKwCVVj9nk", sceGnmUnregisterOwnerAndResources);
    RETAIL("k8EXkhIP+lM", sceGnmUnregisterResource);
    RETAIL("9Mv61HaMhfA", sceGnmRegisterGdsResource);
    RETAIL("6IMbpR7nTzA", sceGnmQueryResourceRegistrationUserMemoryRequirements);
#undef RETAIL

}
} // namespace Core::HostRuntime
