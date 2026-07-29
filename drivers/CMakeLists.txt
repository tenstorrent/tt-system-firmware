#  Copyright (c) 2026 Tenstorrent AI ULC
#  SPDX-License-Identifier: Apache-2.0

add_subdirectory(misc)

# zephyr-keep-sorted-start
add_subdirectory_ifdef(CONFIG_CLOCK_CONTROL clock_control)
add_subdirectory_ifdef(CONFIG_CONSOLE console)
add_subdirectory_ifdef(CONFIG_DMA dma)
add_subdirectory_ifdef(CONFIG_FLASH flash)
add_subdirectory_ifdef(CONFIG_GPIO gpio)
add_subdirectory_ifdef(CONFIG_JTAG jtag)
add_subdirectory_ifdef(CONFIG_MBOX mbox)
add_subdirectory_ifdef(CONFIG_MFD mfd)
add_subdirectory_ifdef(CONFIG_PLDM pmci)
add_subdirectory_ifdef(CONFIG_PWM pwm)
add_subdirectory_ifdef(CONFIG_RESET reset)
add_subdirectory_ifdef(CONFIG_SENSOR sensor)
add_subdirectory_ifdef(CONFIG_SERIAL serial)
add_subdirectory_ifdef(CONFIG_SMBUS_TARGET smbus/target)
add_subdirectory_ifdef(CONFIG_WATCHDOG watchdog)
# zephyr-keep-sorted-stop
