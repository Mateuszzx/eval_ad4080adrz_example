cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/drivers/ad4080)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ad4080_example)

target_sources(app PRIVATE src/main.c)
