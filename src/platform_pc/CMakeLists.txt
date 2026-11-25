# PC hardware abstraction layer + host executables of the Platinum port.
#
#   ppl_platform      host HAL (SDL2, OpenGL, files, save, input, audio)
#   ppl_nitro         NitroSDK/NitroSystem API on the HAL (real SDK headers)
#   ppl_m2_selftest   acceptance test of the Nitro layer (CTest)
#   pokeplatinum      the game
#
# Data layers are looked up at run time: mods/<lang>, mods, pc_data/<lang>
# (extracted from the player's own ROM on first run).
option(PPL_PC_CONSOLE "Keep a console window for logs on Windows" ON)

add_library(ppl_platform STATIC
    os_pc.c
    fs_pc.c
    input_pc.c
    config_pc.c
    install_pc.c
    save_pc.c
    gfx_pc.c
    png_pc.c
    audio_pc.c
    log_pc.c
    hw_pc.c
    crash_pc.c
)
target_include_directories(ppl_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ppl_platform PUBLIC ppl::SDL2 OpenGL::GL)
target_compile_definitions(ppl_platform PUBLIC PLATFORM_PC=1 PPL_REPO_DIR="${PROJECT_SOURCE_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ppl_platform PRIVATE -Wall -Wextra -Wno-unused-parameter)
elseif(MSVC)
    target_compile_options(ppl_platform PRIVATE /W3)
    target_compile_definitions(ppl_platform PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# --- Nitro compatibility layer --------------------------------------------------
# NitroSDK/NitroSystem API implemented on the HAL, plus the SDK/NitroSystem
# sources that are plain C and compile for the host unchanged.
set(_sdk ${PPL_SDK}/libraries)
set(_nns ${PPL_NNS}/libraries)
set(PPL_NITRO_SDK_SOURCES
    ${_sdk}/os/src/os_alloc.c
    ${_sdk}/rtc/src/convert.c
    ${_nns}/fnd/src/allocator.c
    ${_nns}/fnd/src/expheap.c
    ${_nns}/fnd/src/frameheap.c
    ${_nns}/fnd/src/heapcommon.c
    ${_nns}/fnd/src/list.c
    ${_nns}/fnd/src/unitheap.c
    ${_nns}/fnd/src/archive.c)
add_library(ppl_nitro STATIC
    nitro/sdk_asm_pc.c
    nitro/mi_dma_pc.c
    nitro/vram_pc.c
    nitro/ppu_pc.c
    nitro/gpu3d_pc.c
    nitro/snd_pc.c
    nitro/wm_pc.c
    nitro/dgt_pc.c
    nitro/misc_nitro_pc.c
    ${PPL_GENERATED_DIR}/pc_overlay_ids.c
    nitro/os_extra_pc.c
    nitro/nitro_pc.c
    nitro/os_nitro_pc.c
    nitro/fs_nitro_pc.c
    nitro/input_nitro_pc.c
    nitro/card_nitro_pc.c
    nitro/rtc_nitro_pc.c
    nitro/mi_nitro_pc.c
    ${PPL_NITRO_SDK_SOURCES}
)
set_source_files_properties(${PPL_GENERATED_DIR}/pc_overlay_ids.c PROPERTIES GENERATED TRUE)
target_include_directories(ppl_nitro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/nitro)
target_link_libraries(ppl_nitro PUBLIC ppl_game_flags ppl_platform)

# --- M2 acceptance test: game heap + NARC code on the Nitro layer ----------
add_executable(ppl_m2_selftest
    tests/m2_selftest.c                  # includes src/narc.c
    ${PROJECT_SOURCE_DIR}/src/heap.c)
target_link_libraries(ppl_m2_selftest PRIVATE ppl_nitro)
target_compile_options(ppl_m2_selftest PRIVATE "-include${PROJECT_SOURCE_DIR}/include/pch/global_pch.h")
ppl_low_address_link(ppl_m2_selftest)
if(APPLE)
    # The game hooks are weak references (nitro_pc.c); ld64 needs them allowed undefined.
    target_link_options(ppl_m2_selftest PRIVATE -Wl,-undefined,dynamic_lookup)
endif()
if(MINGW)
    target_link_options(ppl_m2_selftest PRIVATE -static)
endif()
set_target_properties(ppl_m2_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME m2_selftest
    COMMAND ppl_m2_selftest --save ${CMAKE_BINARY_DIR}/m2_selftest_save.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# --- The game ------------------------------------------------------------------
include(PplGameSources)
# Checked-in generated sources (see generated/README.md): DS assembly data
# converted to C, and offline stubs for online / unsupported hardware.
file(GLOB PPL_ASM_DATA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/asm_data/*.c)
file(GLOB PPL_STUB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/stubs/*.c)
# Trainer AI scripts: data-only assembly (gTrainerAITable), assembled for the
# host as make_script_bin.sh does for the DS.
set(PPL_TRAINER_AI_OBJ ${CMAKE_BINARY_DIR}/trainer_ai_script${CMAKE_C_OUTPUT_EXTENSION})
add_custom_command(
    OUTPUT ${PPL_TRAINER_AI_OBJ}
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pc_port/asm_data.py
            --cc ${CMAKE_C_COMPILER} --enumproc $<TARGET_FILE:ppl_tool_enumproc>
            -I ${PROJECT_SOURCE_DIR}/include -I ${PROJECT_SOURCE_DIR}/asm -I ${PPL_GENERATED_DIR}
            ${PROJECT_SOURCE_DIR}/src/battle/trainer_ai/script.s ${PPL_TRAINER_AI_OBJ}
    DEPENDS ${PROJECT_SOURCE_DIR}/src/battle/trainer_ai/script.s ${PROJECT_SOURCE_DIR}/asm/macros/aicmd.inc
            ${PROJECT_SOURCE_DIR}/tools/pc_port/asm_data.py ppl_tool_enumproc ppl_generated_headers
    COMMENT "Assembling the trainer AI scripts for the host"
    VERBATIM)
set_source_files_properties(${PPL_TRAINER_AI_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(pokeplatinum
    game_main_pc.c
    game_test_hooks_pc.c
    ${PPL_GAME_SOURCES}
    ${PPL_ASM_DATA_SOURCES}
    ${PPL_STUB_SOURCES}
    ${PPL_TRAINER_AI_OBJ})
target_link_libraries(pokeplatinum PRIVATE ppl_nitro)
set_property(SOURCE game_test_hooks_pc.c APPEND PROPERTY COMPILE_OPTIONS
    "-include${PROJECT_SOURCE_DIR}/include/pch/global_pch.h")
add_dependencies(pokeplatinum ppl_generated_headers)
add_dependencies(ppl_nitro ppl_generated_headers)
ppl_low_address_link(pokeplatinum)
if(MINGW)
    target_link_options(pokeplatinum PRIVATE -static)
endif()
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # lld (llvm-mingw) only: GNU ld (Linux) and ld64 (macOS) reject these.
    target_link_options(pokeplatinum PRIVATE -Wl,--error-limit=0 -Wl,--no-demangle)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Decompiled code: keep the build readable, not pedantic.
    target_compile_options(pokeplatinum PRIVATE -w)
endif()
if(WIN32 AND NOT PPL_PC_CONSOLE)
    set_target_properties(pokeplatinum PROPERTIES WIN32_EXECUTABLE ON)
endif()
set_target_properties(pokeplatinum PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# --- Overlay variables: fresh on every load, as on the DS ---------------------
# Each overlay's sources get their own .data section (Clang pragma), so the
# Nitro layer can snapshot it at start-up and restore it in FS_LoadOverlay*.
# See tools/pc_port/gen_overlay_sections.py.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_overlay_sections.py
                ${PPL_GENERATED_DIR}
        RESULT_VARIABLE _ovl_result
        OUTPUT_VARIABLE _ovl_output
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _ovl_result EQUAL 0)
        message(FATAL_ERROR "gen_overlay_sections.py failed: ${_ovl_output}")
    endif()
    message(STATUS "Overlay sections: ${_ovl_output}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/platinum.us/main.lsf.in
        ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_overlay_sections.py)
    include(${PPL_GENERATED_DIR}/pc_overlay_sources.cmake)
    target_sources(ppl_nitro PRIVATE ${PPL_GENERATED_DIR}/pc_overlay_sections.c)
    target_compile_definitions(ppl_nitro PRIVATE PPL_OVERLAY_RESET=1)

    # Soft reset (OS_ResetSystem): every other game variable goes to
    # ".data$main$1". Overlay sources add their own -include after this one.
    set(_ppl_main_section -fno-zero-initialized-in-bss "-include${PPL_GENERATED_DIR}/pc_ovl/main.h")
    target_compile_options(pokeplatinum PRIVATE ${_ppl_main_section})
    set_property(SOURCE
        nitro/os_nitro_pc.c nitro/os_extra_pc.c nitro/mi_dma_pc.c
        nitro/sdk_asm_pc.c nitro/input_nitro_pc.c nitro/card_nitro_pc.c
        ${PPL_NITRO_SDK_SOURCES}
        APPEND PROPERTY COMPILE_OPTIONS ${_ppl_main_section})
endif()
