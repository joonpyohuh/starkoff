# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(starkoff_ocpp_cp)

target_include_directories(app PRIVATE src lib/jsmn)
FILE(GLOB app_sources src/*.c src/*/*.c)
target_sources(app PRIVATE ${app_sources})
