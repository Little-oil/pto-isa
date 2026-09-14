/**
Copyright (c) 2026 Huawei Technologies Co., Ltd.
This program is free software, you can redistribute it and/or modify it under the terms and conditions of
CANN Open Software License Agreement Version 2.0 (the "License").
Please refer to the License for details. You may not use this file except in compliance with the License.
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
See LICENSE in the root of the software repository for the full text of the License.
*/

#ifndef PTO_CPU_FFTS_HPP
#define PTO_CPU_FFTS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <pto/common/cpu_stub.hpp>

inline constexpr uint16_t FFTS_MODE_VAL = 2;

namespace pto {
static inline uint16_t getFFTSMsg(uint16_t mode, uint16_t eventId, uint16_t baseCount = 1)
{
    return (baseCount & 0xf) | ((mode & 0x3) << 4) | ((eventId & 0xf) << 8);
}
} // namespace pto

namespace pto::cpu_sim::ffts {

// A2/A3 mode-2 events for one cube and two vector lanes. The runtime owns the
// zero-initialized storage and its device/cluster isolation and reset lifetime.
// This models event ordering, not hardware pipeline timing or counter capacity.
struct EventState {
    std::atomic<uint32_t> cubeToVector[2]{};
    std::atomic<uint32_t> vectorToCube[2]{};
};
static_assert(std::is_trivially_destructible_v<EventState>);

struct EventStorage {
    uint32_t initialized;
    alignas(EventState) unsigned char payload[sizeof(EventState)];
};

static inline bool IsCube()
{
#if defined(__DAV_CUBE__) && !defined(__DAV_VEC__)
    return true;
#elif defined(__DAV_VEC__) && !defined(__DAV_CUBE__)
    return false;
#else
    throw std::runtime_error("FFTS simulation requires an AIC or AIV kernel specialization");
#endif
}

static inline EventState& GetEventState(int eventId)
{
    if (eventId < 0 || eventId >= 16) {
        throw std::runtime_error("FFTS simulation requires an event ID in [0, 15]");
    }
    if (injected_pipe_shared_state_hook == nullptr || injected_subblock_id_hook == nullptr) {
        throw std::runtime_error("FFTS simulation requires runtime hook injection");
    }
    // Keep explicit events outside the tile-pipe state key namespace.
    constexpr uint64_t keyPrefix = 0xff46465453000000ULL;
    auto* storage =
        static_cast<EventStorage*>(injected_pipe_shared_state_hook(keyPrefix | eventId, sizeof(EventStorage)));
    if (storage == nullptr) {
        throw std::runtime_error("FFTS simulation requires runtime shared storage");
    }
    std::atomic_ref<uint32_t> initialized(storage->initialized);
    uint32_t expected = 0;
    if (initialized.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        new (storage->payload) EventState{};
        initialized.store(2, std::memory_order_release);
    } else {
        while (initialized.load(std::memory_order_acquire) != 2) {
        }
    }
    return *std::launder(reinterpret_cast<EventState*>(storage->payload));
}

static inline uint32_t VectorLane()
{
    const uint32_t lane = injected_subblock_id_hook();
    if (lane >= 2) {
        throw std::runtime_error("FFTS mode 2 requires AIV subblock 0 or 1");
    }
    return lane;
}

static inline void Publish(std::atomic<uint32_t>& credits)
{
    uint32_t value = credits.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("FFTS simulation credit counter overflow");
        }
    } while (!credits.compare_exchange_weak(value, value + 1, std::memory_order_release, std::memory_order_relaxed));
}

static inline void Consume(std::atomic<uint32_t>& credits)
{
    uint32_t value = credits.load(std::memory_order_acquire);
    for (;;) {
        if (value == 0) {
            std::this_thread::yield();
            value = credits.load(std::memory_order_acquire);
        } else if (credits.compare_exchange_weak(value, value - 1, std::memory_order_acquire)) {
            return;
        }
    }
}

static inline void Signal(uint16_t message)
{
    if (((message >> 4) & 0x3) != FFTS_MODE_VAL || (message & 0xf) != 1) {
        throw std::runtime_error("FFTS simulation supports mode 2 with base count 1 only");
    }
    const bool isCube = IsCube();
    auto& state = GetEventState((message >> 8) & 0xf);
    if (isCube) {
        Publish(state.cubeToVector[0]);
        Publish(state.cubeToVector[1]);
    } else {
        Publish(state.vectorToCube[VectorLane()]);
    }
}

static inline void Wait(int eventId)
{
    const bool isCube = IsCube();
    auto& state = GetEventState(eventId);
    if (isCube) {
        // Two signals from one vector lane cannot replace the other lane.
        Consume(state.vectorToCube[0]);
        Consume(state.vectorToCube[1]);
    } else {
        Consume(state.cubeToVector[VectorLane()]);
    }
}
} // namespace pto::cpu_sim::ffts

static inline void __builtin_cce_ffts_cross_core_sync(int, uint16_t message) { pto::cpu_sim::ffts::Signal(message); }

static inline void __builtin_cce_wait_flag_dev(int eventId) { pto::cpu_sim::ffts::Wait(eventId); }

static inline void ffts_cross_core_sync(int pipe, uint16_t message)
{
    __builtin_cce_ffts_cross_core_sync(pipe, message);
}

static inline void wait_flag_dev(int eventId) { __builtin_cce_wait_flag_dev(eventId); }
#endif // PTO_CPU_FFTS_HPP
