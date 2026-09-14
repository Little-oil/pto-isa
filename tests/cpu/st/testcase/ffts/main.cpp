/**
Copyright (c) 2026 Huawei Technologies Co., Ltd.
This program is free software, you can redistribute it and/or modify it under the terms and conditions of
CANN Open Software License Agreement Version 2.0 (the "License").
Please refer to the License for details. You may not use this file except in compliance with the License.
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
See LICENSE in the root of the software repository for the full text of the License.
*/

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <gtest/gtest.h>
#include <pto/pto-inst.hpp>

namespace {
using namespace std::chrono_literals;
thread_local uint32_t laneId = 0;
std::mutex storageMutex;
std::unordered_map<uint64_t, void*> sharedStorage;

uint32_t GetLane() { return laneId; }

void* GetStorage(uint64_t key, size_t size)
{
    std::lock_guard<std::mutex> lock(storageMutex);
    auto [it, inserted] = sharedStorage.try_emplace(key, nullptr);
    if (inserted) {
        it->second = std::calloc(1, size);
    }
    return it->second;
}

struct Kernel {
    void* handle;
    void (*configure)(void*, void*);
    void (*signal)(int, int, int);
    void (*wait)(int);
    void (*legacySignal)(int);
    void (*legacyWait)(int);

    explicit Kernel(const char* path) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        if (handle == nullptr) {
            throw std::runtime_error(dlerror());
        }
        configure = Lookup<decltype(configure)>("configure");
        signal = Lookup<decltype(signal)>("signal_event");
        wait = Lookup<decltype(wait)>("wait_event");
        legacySignal = Lookup<decltype(legacySignal)>("legacy_signal_event");
        legacyWait = Lookup<decltype(legacyWait)>("legacy_wait_event");
        configure(reinterpret_cast<void*>(GetLane), reinterpret_cast<void*>(GetStorage));
    }

    template <typename Fn>
    Fn Lookup(const char* name)
    {
        auto symbol = dlsym(handle, name);
        if (symbol == nullptr) {
            throw std::runtime_error(dlerror());
        }
        return reinterpret_cast<Fn>(symbol);
    }

    ~Kernel() { dlclose(handle); }
};

class Waiter {
public:
    template <typename Fn>
    explicit Waiter(Fn fn) : result_(std::async(std::launch::async, fn))
    {}

    void ExpectBlocked() { EXPECT_EQ(result_.wait_for(20ms), std::future_status::timeout); }

    void Finish()
    {
        if (result_.wait_for(5s) != std::future_status::ready) {
            ADD_FAILURE() << "FFTS waiter did not receive its matching events";
            std::abort();
        }
        result_.get();
    }

private:
    std::future<void> result_;
};

class FftsTest : public testing::Test {
protected:
    Kernel cube{FFTS_AIC_LIBRARY};
    Kernel vector{FFTS_AIV_LIBRARY};

    void TearDown() override
    {
        for (auto [key, storage] : sharedStorage) {
            std::free(storage);
        }
        sharedStorage.clear();
        laneId = 0;
    }
};

TEST_F(FftsTest, MessageEncoding)
{
    EXPECT_EQ(pto::getFFTSMsg(FFTS_MODE_VAL, 7), 0x721);
    EXPECT_EQ(pto::getFFTSMsg(1, 15, 3), 0xf13);
}

TEST_F(FftsTest, UnspecializedTranslationUnitRejectsExecution)
{
    EXPECT_THROW(__builtin_cce_ffts_cross_core_sync(PIPE_FIX, pto::getFFTSMsg(2, 0)), std::runtime_error);
    EXPECT_THROW(__builtin_cce_wait_flag_dev(0), std::runtime_error);
}

TEST_F(FftsTest, BroadcastPreservesQueuedCredits)
{
    for (int i = 0; i < 3; ++i) {
        cube.signal(0, 2, 1);
    }
    for (laneId = 0; laneId < 2; ++laneId) {
        for (int i = 0; i < 3; ++i) {
            vector.wait(0);
        }
    }
    Waiter first([&] {
        laneId = 0;
        vector.wait(0);
    });
    Waiter second([&] {
        laneId = 1;
        vector.legacyWait(0);
    });
    first.ExpectBlocked();
    second.ExpectBlocked();
    cube.legacySignal(0);
    first.Finish();
    second.Finish();
}

TEST_F(FftsTest, JoinRequiresBothLanesAndMatchingEvent)
{
    laneId = 0;
    vector.signal(3, 2, 1);
    vector.signal(3, 2, 1);
    Waiter first([&] { cube.wait(3); });
    first.ExpectBlocked();
    laneId = 1;
    vector.signal(7, 2, 1);
    first.ExpectBlocked();
    vector.signal(3, 2, 1);
    first.Finish();
    Waiter second([&] { cube.legacyWait(3); });
    second.ExpectBlocked();
    vector.legacySignal(3);
    second.Finish();
}

TEST_F(FftsTest, SynchronizationPublishesPayloadAcrossLibraries)
{
    int input = 0;
    std::array<int, 2> output{};
    Waiter producer([&] {
        for (int epoch = 1; epoch <= 100; ++epoch) {
            input = epoch;
            cube.signal(0, 2, 1);
            cube.wait(1);
            EXPECT_EQ(output[0], epoch * 2);
            EXPECT_EQ(output[1], epoch * 2 + 1);
        }
    });
    auto consume = [&](uint32_t lane) {
        laneId = lane;
        for (int epoch = 1; epoch <= 100; ++epoch) {
            vector.wait(0);
            output[lane] = input * 2 + lane;
            vector.signal(1, 2, 1);
        }
    };
    Waiter first([&] { consume(0); });
    Waiter second([&] { consume(1); });
    producer.Finish();
    first.Finish();
    second.Finish();
}

TEST_F(FftsTest, RejectsUnsupportedModeCountAndWaitId)
{
    for (int mode : {0, 1, 3}) {
        EXPECT_THROW(cube.signal(0, mode, 1), std::runtime_error);
        EXPECT_THROW(vector.signal(0, mode, 1), std::runtime_error);
    }
    for (int count : {0, 2, 15}) {
        EXPECT_THROW(cube.signal(0, 2, count), std::runtime_error);
        EXPECT_THROW(vector.signal(0, 2, count), std::runtime_error);
    }
    for (int event : {-1, 16}) {
        EXPECT_THROW(cube.wait(event), std::runtime_error);
        EXPECT_THROW(vector.wait(event), std::runtime_error);
    }
}

TEST_F(FftsTest, RequiresRuntimeStorageAndValidVectorLane)
{
    cube.configure(nullptr, nullptr);
    EXPECT_THROW(cube.signal(0, 2, 1), std::runtime_error);
    auto nullStorage = +[](uint64_t, size_t) -> void* { return nullptr; };
    cube.configure(reinterpret_cast<void*>(GetLane), reinterpret_cast<void*>(nullStorage));
    EXPECT_THROW(cube.wait(0), std::runtime_error);
    laneId = 2;
    EXPECT_THROW(vector.signal(0, 2, 1), std::runtime_error);
    EXPECT_THROW(vector.wait(0), std::runtime_error);
}
} // namespace
