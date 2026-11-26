# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.16)

# Default to the repository's local Voice firmware for the Waveshare 1.75C.
# Explicit SDKCONFIG_DEFAULTS still selects a different upstream profile.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
    set(SDKCONFIG_DEFAULTS
        sdkconfig.defaults
        devices/sdkconfig.muse
        devices/sdkconfig.muse-waveshare-s3-175c
        devices/sdkconfig.local-voice-175c)
    if(NOT DEFINED IDF_TARGET AND "$ENV{IDF_TARGET}" STREQUAL "")
        set(IDF_TARGET esp32s3)
    endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/project.cmake")
project(muse-gadget)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/validate_config.cmake")

if(CONFIG_MUSE_LOCAL_VOICE AND CONFIG_BT_NIMBLE_MAX_BONDS EQUAL 1
   AND IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 1)
    # GCC 15 diagnoses NimBLE's NVS bond-order check even though the
    # single-bond store cannot enter its sorting loop.
    idf_component_get_property(bt_target bt COMPONENT_LIB)
    set_source_files_properties(
        "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/store/config/src/ble_store_nvs.c"
        TARGET_DIRECTORY ${bt_target} PROPERTIES COMPILE_OPTIONS "-Wno-error=array-bounds")
endif()
