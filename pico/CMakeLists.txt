# PicoWikiGuide firmware for the PicoCalc (Raspberry Pi Pico 2 / 2 W)
#
#   export PICO_SDK_PATH=/path/to/pico-sdk      (version 2.x)
#   cmake -S pico -B build-pico -DPICO_BOARD=pico2
#   make -C build-pico -j
#
# makes build-pico/pwg_hwtest.uf2 (hardware test)
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(picowikiguide C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(TOP ${CMAKE_CURRENT_LIST_DIR}/..)
include_directories(${TOP}/src ${TOP}/fonts ${CMAKE_CURRENT_LIST_DIR})

add_executable(pwg_hwtest
	hwtest.c lcd.c kbd.c sd.c
	${TOP}/src/font.c ${TOP}/src/inflate.c ${TOP}/src/gfx.c ${TOP}/src/fat.c ${TOP}/src/blockdev.c
	${TOP}/src/overlay.c ${TOP}/src/present.c ${TOP}/fonts/fonts.c)
target_link_libraries(pwg_hwtest pico_stdlib hardware_spi hardware_i2c)
pico_enable_stdio_usb(pwg_hwtest 0)
pico_enable_stdio_uart(pwg_hwtest 0)
pico_add_extra_outputs(pwg_hwtest)

# the Unicode fallback font goes into flash (read in place, no RAM)
set(WIKIFONT ${TOP}/fonts/WIKIFONT.BIN)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wikifont.S
	".section .rodata.wikifont,\"a\"\n.balign 4\n.global wikifont_data\nwikifont_data:\n"
	".incbin \"${WIKIFONT}\"\n.global wikifont_end\nwikifont_end:\n")
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/wikifont.S PROPERTIES OBJECT_DEPENDS ${WIKIFONT})

# the firmware
add_executable(picowikiguide
	plat_pico.c plat_storage.c dictflash.c ${TOP}/src/dictsync.c lcd.c kbd.c sd.c ${CMAKE_CURRENT_BINARY_DIR}/wikifont.S
	${TOP}/src/app.c ${TOP}/src/splash.c ${TOP}/src/wiki.c ${TOP}/src/index.c ${TOP}/src/fold.c
	${TOP}/src/calc.c ${TOP}/src/font.c ${TOP}/src/inflate.c ${TOP}/src/gfx.c ${TOP}/src/fat.c
	${TOP}/src/blockdev.c ${TOP}/fonts/fonts.c ${TOP}/third_party/zstd/zstddeclib.c
	${TOP}/src/overlay.c ${TOP}/src/present.c ${TOP}/third_party/tjpgd/tjpgd.c)
target_include_directories(picowikiguide PRIVATE ${TOP}/third_party/zstd ${TOP}/third_party/tjpgd)
target_compile_definitions(picowikiguide PRIVATE ZSTD_LEGACY_SUPPORT=0 ZSTD_DISABLE_ASM
	ZSTD_NO_INLINE=1 PICO_STACK_SIZE=0x1000
	PICO_MALLOC_PANIC=0 PWG_DICT_IN_FLASH=1)
target_link_libraries(picowikiguide pico_stdlib hardware_spi hardware_i2c hardware_watchdog hardware_flash)
pico_enable_stdio_usb(picowikiguide 0)
pico_enable_stdio_uart(picowikiguide 0)
pico_add_extra_outputs(picowikiguide)
