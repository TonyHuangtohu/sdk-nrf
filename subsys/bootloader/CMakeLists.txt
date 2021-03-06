#
# Copyright (c) 2018 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

add_subdirectory(bl_boot)

zephyr_include_directories(include)

if (NOT IMAGE_NAME)
  message(WARNING "Building bootloader without associated application. "
          "Dummy values will be used for partition configuration."
          "The idiomatic way of including the immutable bootloader is to set "
          "the configuration 'CONFIG_SECURE_BOOTLOADER=y' in your applications "
          "'prj.conf'.")
  zephyr_include_directories(include/dummy_values/)
endif()

if (DEFINED CONFIG_SOC_NRF9160 OR DEFINED CONFIG_SOC_NRF5340_CPUAPP)
  message(WARNING "
      --------------------------------------------------------
      --- WARNING: When using the immutable bootloader on  ---
      --- this SoC, the UICR must be erased when flashing. ---
      --- E.g. by calling 'west flash --erase'             ---
      --------------------------------------------------------")
endif()
