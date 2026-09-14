/**
Copyright (c) 2026 Huawei Technologies Co., Ltd.
This program is free software, you can redistribute it and/or modify it under the terms and conditions of
CANN Open Software License Agreement Version 2.0 (the "License").
Please refer to the License for details. You may not use this file except in compliance with the License.
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
See LICENSE in the root of the software repository for the full text of the License.
*/

#include <cstddef>
#include <cstdint>
#include <pto/pto-inst.hpp>

extern "C" void configure(void* lane, void* storage) { pto::cpu_sim::register_hooks(lane, storage); }

extern "C" void signal_event(int event, int mode, int count)
{
    __builtin_cce_ffts_cross_core_sync(PIPE_MTE3, pto::getFFTSMsg(mode, event, count));
}

extern "C" void wait_event(int event) { __builtin_cce_wait_flag_dev(event); }

extern "C" void legacy_signal_event(int event)
{
    ffts_cross_core_sync(PIPE_FIX, pto::getFFTSMsg(FFTS_MODE_VAL, event));
}

extern "C" void legacy_wait_event(int event) { wait_flag_dev(event); }
