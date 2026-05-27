cmake_minimum_required(VERSION 3.13)

# Board must be set before the SDK is included.
set(PICO_BOARD wiznet_w5500_evb_pico CACHE STRING "Target board")

# The VS Code Pico extension sets PICO_SDK_PATH and passes it through cmake;
# for command-line builds copy pico_sdk_import.cmake from
#   $PICO_SDK_PATH/external/pico_sdk_import.cmake  into this directory.
if(DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
else()
    include(pico_sdk_import.cmake)
endif()

project(thl_pwr_ctl C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

# ---------------------------------------------------------------------------
# WizNet ioLibrary_Driver
#   git clone --depth=1 https://github.com/Wiznet/ioLibrary_Driver.git lib/ioLibrary_Driver
# ---------------------------------------------------------------------------
set(IOLIB ${CMAKE_SOURCE_DIR}/lib/ioLibrary_Driver)

add_library(ioLibrary_Ethernet STATIC
    ${IOLIB}/Ethernet/W5500/w5500.c
    ${IOLIB}/Ethernet/wizchip_conf.c
    ${IOLIB}/Ethernet/socket.c
)
target_include_directories(ioLibrary_Ethernet PUBLIC
    ${IOLIB}/Ethernet
    ${IOLIB}/Ethernet/W5500
)

# ---------------------------------------------------------------------------
# Shared hardware port layer (SPI + W5500 init)
# ---------------------------------------------------------------------------
add_library(w5500_port STATIC
    src/w5500_port.c
)
target_include_directories(w5500_port PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(w5500_port
    pico_stdlib
    hardware_spi
    ioLibrary_Ethernet
)

# ---------------------------------------------------------------------------
# hello_world — minimal HTTP server for initial hardware validation
# ---------------------------------------------------------------------------
add_executable(hello_world
    src/hello_world.c
)
target_include_directories(hello_world PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(hello_world
    w5500_port
    pico_stdlib
    ioLibrary_Ethernet
)
pico_enable_stdio_usb(hello_world 1)
pico_enable_stdio_uart(hello_world 0)
pico_add_extra_outputs(hello_world)

# ---------------------------------------------------------------------------
# ssd1306 — shared OLED driver library
# ---------------------------------------------------------------------------
add_library(ssd1306 STATIC
    src/ssd1306.c
)
target_include_directories(ssd1306 PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ssd1306
    pico_stdlib
    hardware_i2c
)

# ---------------------------------------------------------------------------
# self_test — PCB self-test firmware (OLED + subcomponent checks)
# Override I2C pins at build time if your PCB differs from the defaults:
#   cmake -DSELF_TEST_SDA=14 -DSELF_TEST_SCL=15 ...
# ---------------------------------------------------------------------------
if(NOT DEFINED SELF_TEST_SDA)
    set(SELF_TEST_SDA 2)   # GP2 = I2C1 SDA (PCB pin mapping)
endif()
if(NOT DEFINED SELF_TEST_SCL)
    set(SELF_TEST_SCL 3)   # GP3 = I2C1 SCL (PCB pin mapping)
endif()

add_executable(self_test
    src/self_test.c
)
target_include_directories(self_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(self_test PRIVATE
    SSD1306_SDA_PIN=${SELF_TEST_SDA}
    SSD1306_SCL_PIN=${SELF_TEST_SCL}
)
target_link_libraries(self_test
    ssd1306
    pico_stdlib
    hardware_i2c
    hardware_uart
    hardware_spi
)
pico_enable_stdio_usb(self_test 1)
pico_enable_stdio_uart(self_test 0)
pico_add_extra_outputs(self_test)

# ---------------------------------------------------------------------------
# thl_pwr_ctl — full firmware with web config UI and flash persistence
# ---------------------------------------------------------------------------
add_executable(thl_pwr_ctl
    src/main.c
    src/config.c
    src/http_server.c
    src/w5500_port.c
)
target_include_directories(thl_pwr_ctl PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(thl_pwr_ctl
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_watchdog
    ioLibrary_Ethernet
)
pico_enable_stdio_usb(thl_pwr_ctl 1)
pico_enable_stdio_uart(thl_pwr_ctl 0)
pico_add_extra_outputs(thl_pwr_ctl)

# ---------------------------------------------------------------------------
# pwr_ctl_test — Single-channel power control subsystem test
#   MCP3428 ADC comms check + GP27 enable toggle, results on SSD1306 OLED
# ---------------------------------------------------------------------------
add_executable(pwr_ctl_test
    src/pwr_ctl_test.c
)
target_include_directories(pwr_ctl_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(pwr_ctl_test
    ssd1306
    pico_stdlib
    hardware_i2c
)
pico_enable_stdio_usb(pwr_ctl_test 1)
pico_enable_stdio_uart(pwr_ctl_test 0)
pico_add_extra_outputs(pwr_ctl_test)

# ---------------------------------------------------------------------------
# comms_test — I2C and SPI device communication test (OLED, EEPROM, GPIO exp,
#              ADC pair, DAC)
# ---------------------------------------------------------------------------
add_executable(comms_test
    src/comms_test.c
)
target_include_directories(comms_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(comms_test
    ssd1306
    pico_stdlib
    hardware_i2c
    hardware_spi
)
pico_enable_stdio_usb(comms_test 1)
pico_enable_stdio_uart(comms_test 0)
pico_add_extra_outputs(comms_test)

# ---------------------------------------------------------------------------
# relay_gpio_test — Relay driver and GPIO subsystem PoC test
#   TCAL9539 GPIO expander + TLC59108 LED driver + GP22 direct relay
#   Auto-detects I2C bus (I2C0/I2C1) at boot via physical jumpers
# ---------------------------------------------------------------------------
add_executable(relay_gpio_test
    src/relay_gpio_test.c
)
target_include_directories(relay_gpio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(relay_gpio_test
    ssd1306
    pico_stdlib
    hardware_i2c
)
pico_enable_stdio_usb(relay_gpio_test 1)
pico_enable_stdio_uart(relay_gpio_test 0)
pico_add_extra_outputs(relay_gpio_test)
