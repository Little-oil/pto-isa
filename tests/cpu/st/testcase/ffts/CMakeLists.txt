# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.

pto_cpu_sim_st(ffts)
foreach(core aic aiv)
    add_library(ffts_${core} SHARED kernel.cpp)
    target_include_directories(ffts_${core} PRIVATE ${PROJECT_SOURCE_DIR}/../../../include)
    if(core STREQUAL "aic")
        target_compile_definitions(ffts_${core} PRIVATE __DAV_CUBE__)
    else()
        target_compile_definitions(ffts_${core} PRIVATE __DAV_VEC__)
    endif()
    target_compile_options(ffts_${core} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
    add_dependencies(ffts ffts_${core})
endforeach()
target_compile_definitions(ffts PRIVATE
    FFTS_AIC_LIBRARY="$<TARGET_FILE:ffts_aic>"
    FFTS_AIV_LIBRARY="$<TARGET_FILE:ffts_aiv>")
