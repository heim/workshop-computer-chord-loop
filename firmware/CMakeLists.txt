# ChordLoop card for the Music Thing Modular Workshop System Computer.
#
# Requires the Pico SDK (>= 2.1) with PICO_SDK_PATH set, and ComputerCard.h in
# this directory (vendored from Workshop_Computer PicoSDK/ComputerCard/).
#
#   cmake -S firmware -B firmware/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build firmware/build --parallel
#
# The .uf2 is written to firmware/build/chordloop.uf2.

cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(chordloop C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(chordloop
    main.cpp
    chord_engine.cpp
    music_tables.cpp
    arp.cpp
    pad_voice.cpp
    serial_proto.cpp
    persist.cpp
    persist_flash.cpp
    usb_descriptors.c
)

target_include_directories(chordloop PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Catch accidental double-precision math in the audio path.
target_compile_options(chordloop PRIVATE -Wall -Wextra -Wdouble-promotion -Wfloat-conversion)

# Some boards need extra time for the crystal oscillator to start.
target_compile_definitions(chordloop PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

# Run the entire binary from RAM. ProcessSample() has a hard ~3000-cycle budget
# at 144 MHz; XIP cache misses on the (cold) chord-clock path were enough to
# overrun it, which scrambles ComputerCard's ADC mux sequencing - knobs, switch
# and CV inputs all misread. ~90 KB total fits RAM comfortably, and it also
# means the audio core never touches flash while core1 writes settings.
pico_set_binary_type(chordloop copy_to_ram)

target_link_libraries(chordloop
    pico_stdlib
    pico_multicore        # USB stack runs on core1
    pico_unique_id        # serial number + RNG seed from flash unique ID
    pico_flash            # flash_safe_execute for settings persistence
    tinyusb_device        # raw TinyUSB CDC (device mode)
    hardware_adc
    hardware_dma
    hardware_pwm
    hardware_spi
    hardware_i2c
    hardware_flash
)

# Raw TinyUSB on core1; keep stdio off USB and UART so nothing else grabs the
# USB controller.
pico_enable_stdio_usb(chordloop 0)
pico_enable_stdio_uart(chordloop 0)

# Generate the .uf2 for BOOTSEL flashing.
pico_add_extra_outputs(chordloop)
