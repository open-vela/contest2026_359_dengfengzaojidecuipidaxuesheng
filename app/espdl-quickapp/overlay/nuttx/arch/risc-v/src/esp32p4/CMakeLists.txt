# ##############################################################################
# arch/risc-v/src/esp32p4/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

# Include common espressif CMake configuration
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/espressif espressif)

set(SRCS esp_chip_rev.c)

if(CONFIG_ESP32P4_PPA OR CONFIG_ESP32P4_JPEG)
  list(APPEND SRCS esp32p4_dma2d.c)
endif()

if(CONFIG_ESP32P4_JPEG)
  set(P4_JPEG_HAL ${CMAKE_CURRENT_SOURCE_DIR}/esp-hal-3rdparty/components/esp_hal_jpeg)
  list(APPEND SRCS esp32p4_jpeg.c esp32p4_jpeg_parse.c esp32p4_jpeg_emit.c
    ${P4_JPEG_HAL}/jpeg_hal.c)
  target_include_directories(arch PRIVATE
    ${P4_JPEG_HAL}/include ${P4_JPEG_HAL}/esp32p4/include)
endif()

if(CONFIG_ESP32P4_PPA)
  list(APPEND SRCS esp32p4_ppa.c)
  target_include_directories(arch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/esp-hal-3rdparty/components/esp_hal_ppa/include
    ${CMAKE_CURRENT_SOURCE_DIR}/esp-hal-3rdparty/components/esp_hal_ppa/esp32p4/include)
endif()

if(CONFIG_SMP)
  list(APPEND SRCS esp32p4_smp.c)
endif()

target_compile_definitions(arch PRIVATE _RETARGETABLE_LOCKING)
target_compile_options(arch PRIVATE -Wno-array-bounds -Wno-stringop-overflow)

if(SRCS)
  target_sources(arch PRIVATE ${SRCS})
endif()
