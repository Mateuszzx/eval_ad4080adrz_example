cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/drivers/ad4080)

# Captured here: the macro runs later, from Zephyr's cmake directory.
set(AD4080_OPENOCD_RESUME ${CMAKE_CURRENT_LIST_DIR}/support/openocd-resume.tcl)

# The board OpenOCD file is the Nucleo one. Connect under reset so a core
# in WFI can be halted, keep the hex erase-program path, and turn the
# trailing "reset run" into resume. That "reset run" drops SWD on this
# ST-Link after the image is already written.
macro(app_set_runner_args)
  board_runner_args(openocd "--cmd-pre-init" "reset_config srst_only srst_nogate connect_assert_srst")
  # "reset" exists only after init.
  board_runner_args(openocd "--cmd-pre-load" "source ${AD4080_OPENOCD_RESUME}")
endmacro()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ad4080_example)

target_sources(app PRIVATE src/main.c src/usbd.c)
