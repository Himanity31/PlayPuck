cmake_minimum_required(VERSION 3.13)

# Include Pico SDK configuration
include(pico_sdk_import.cmake)

project(playpuck C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# --- Spinblade Target (Orbital Action Defender) ---
add_executable(spinblade
    spinblade.cpp
)
target_link_libraries(spinblade
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_dma
)
pico_enable_stdio_usb(spinblade 1)
pico_enable_stdio_uart(spinblade 0)
pico_add_extra_outputs(spinblade)

# --- Safe Cracker Target (Rogue-Lite Safe Dial Combination) ---
add_executable(safecracker
    safecracker.cpp
)
target_link_libraries(safecracker
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_dma
)
pico_enable_stdio_usb(safecracker 1)
pico_enable_stdio_uart(safecracker 0)
pico_add_extra_outputs(safecracker)

# --- Display Diagnostic Test Target ---
add_executable(display_test
    display_test.cpp
)
target_link_libraries(display_test
    pico_stdlib
    hardware_spi
)
pico_enable_stdio_usb(display_test 1)
pico_enable_stdio_uart(display_test 0)
pico_add_extra_outputs(display_test)

# --- PlayPuck High-Performance Video & Display Streamer ---
add_executable(video_player
    video_player.cpp
)
target_link_libraries(video_player
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_i2c
    hardware_dma
    hardware_vreg
    hardware_clocks
)
pico_enable_stdio_usb(video_player 1)
pico_enable_stdio_uart(video_player 0)
pico_add_extra_outputs(video_player)

