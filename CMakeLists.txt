cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Community firmware replaces the bootloader when installed over USB. Keep the
# factory Recovery entry gesture in every derivative build so USB development
# does not strand the device outside the mini-program BLE install path.
idf_build_set_property(
    BOOTLOADER_EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/bootloader_components/"
    APPEND)

project(FoloToy-AI-Passport)

# 叫声库进 cryfs@0x35A000,不进 3 MB 应用。拷进构建目录后挂到 flash/merge-bin,
# 隔离构建(validate.sh --firmware)也能找到文件。
set(POKEDEX_CRIES_SRC "${CMAKE_CURRENT_LIST_DIR}/main/pokedex_cries.bin")
set(POKEDEX_CRIES_IMG "${CMAKE_BINARY_DIR}/pokedex_cries.bin")
if(NOT EXISTS "${POKEDEX_CRIES_SRC}")
    message(FATAL_ERROR "main/pokedex_cries.bin missing; run tools/gen_pokedex_cries.py")
endif()
add_custom_command(OUTPUT "${POKEDEX_CRIES_IMG}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${POKEDEX_CRIES_SRC}" "${POKEDEX_CRIES_IMG}"
    DEPENDS "${POKEDEX_CRIES_SRC}"
    VERBATIM)
add_custom_target(pokedex_cries_image ALL DEPENDS "${POKEDEX_CRIES_IMG}")
esptool_py_flash_to_partition(flash "cryfs" "${POKEDEX_CRIES_IMG}")
add_dependencies(flash pokedex_cries_image)
