cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME DA15)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# TinyUSB paths
set(TINYUSB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Lib/tinyusb")

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Application sources
    "App/Src/app.c"
    "App/Src/display.c"
    "App/Src/audio_output.c"
    "App/Src/audio_eq.c"
    "App/Src/usb_descriptors.c"
    "App/Src/usb_audio.c"
    "App/Src/sh1106.c"
    "App/Src/encoder.c"
    "App/Src/settings.c"
    "App/Src/eq_profile.c"
    "App/Src/usb_comm.c"

    # SEGGER RTT
    "Lib/RTT/SEGGER_RTT.c"
    "Lib/RTT/SEGGER_RTT_printf.c"

    # TinyUSB core
    "${TINYUSB_PATH}/src/tusb.c"
    "${TINYUSB_PATH}/src/device/usbd.c"
    "${TINYUSB_PATH}/src/device/usbd_control.c"
    "${TINYUSB_PATH}/src/common/tusb_fifo.c"

    # TinyUSB Audio class driver
    "${TINYUSB_PATH}/src/class/audio/audio_device.c"

    # TinyUSB DFU Runtime class driver
    "${TINYUSB_PATH}/src/class/dfu/dfu_rt_device.c"

    # TinyUSB CDC class driver
    "${TINYUSB_PATH}/src/class/cdc/cdc_device.c"

    # TinyUSB STM32 FSDEV portable driver (for STM32H5)
    "${TINYUSB_PATH}/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c"
    "${TINYUSB_PATH}/src/portable/st/stm32_fsdev/fsdev_common.c"
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Application includes
    App/Inc

    # SEGGER RTT
    Lib/RTT

    # TinyUSB includes
    "${TINYUSB_PATH}/src"
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # TinyUSB config location
    CFG_TUSB_MCU=OPT_MCU_STM32H5
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    m

    # Add user defined libraries
)

# Generate .bin file from ELF after build
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
)
