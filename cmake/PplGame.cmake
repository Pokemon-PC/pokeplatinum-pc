# Compile settings for game / NitroSDK / NitroSystem code built for the host.
#
#   ppl_game_flags      INTERFACE target: include paths, defines and options
#                       mirroring the DS build (meson.build).
#   ppl_low_address_link(<target>)
#                       links an executable so its code and static data live
#                       below 4 GiB (see src/platform_pc/hw_pc.h for why).
#
# The Nintendo libraries are the subprojects the DS build fetches itself
# (subprojects/*.wrap: decompiled NitroSDK, NitroSystem, NitroDWC, NitroWiFi,
# libvct), at the same revisions. Fetch them once with
#   python tools/pc_port/fetch_subprojects.py
# (git clone of the public repositories named in the .wrap files).

set(PPL_REVISION "1" CACHE STRING "ROM revision of the decomp (0 or 1)")
option(PPL_WARN_PTR_CASTS "Report pointer<->int size-mismatch casts in game code (64-bit audit)" OFF)

set(PPL_SUBPROJECTS ${PROJECT_SOURCE_DIR}/subprojects)
set(PPL_SDK         ${PPL_SUBPROJECTS}/NitroSDK-4.2.30001)
set(PPL_NNS         ${PPL_SUBPROJECTS}/NitroSystem-071126.1)
set(PPL_DWC         ${PPL_SUBPROJECTS}/NitroDWC-2.2.30008)
set(PPL_WIFI        ${PPL_SUBPROJECTS}/NitroWiFi-2.1.30003)
set(PPL_VCT         ${PPL_SUBPROJECTS}/libvct-1.3.1)
set(PPL_YYJSON      ${PPL_SUBPROJECTS}/yyjson-0.12.0)
foreach(_dir ${PPL_SDK} ${PPL_NNS} ${PPL_DWC} ${PPL_WIFI} ${PPL_VCT} ${PPL_YYJSON} ${PPL_SUBPROJECTS}/metang)
    if(NOT EXISTS ${_dir})
        message(FATAL_ERROR "${_dir} is missing. Run: python tools/pc_port/fetch_subprojects.py")
    endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# --- host tools of the repository (data tables compiled into the game) -----
# tools/dataproc turns the JSON data of res/ into C tables (species, items,
# trainers...). Built for the host, as in the DS build.
set(PPL_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated_root)
set(PPL_HOSTTOOLS_DIR ${CMAKE_BINARY_DIR}/hosttools)
# The tools need the metang enumerations (generated/*.h) first.
file(GLOB _ppl_metang_txt CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/generated/*.txt)
add_custom_command(
    OUTPUT ${PPL_GENERATED_DIR}/enums.stamp
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_headers.py
            ${PPL_GENERATED_DIR} --enums-only --stamp ${PPL_GENERATED_DIR}/enums.stamp
    DEPENDS ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_headers.py ${_ppl_metang_txt}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating enumerations (metang)"
    VERBATIM)
add_custom_target(ppl_enum_headers DEPENDS ${PPL_GENERATED_DIR}/enums.stamp)
include(PplHostTools)
add_dependencies(ppl_host_dataproc ppl_enum_headers)

# ROM file system: NARC indices are checked against it when present.
set(PPL_ROM_DATA_DIR "${PROJECT_SOURCE_DIR}/pc_data/fr" CACHE PATH
    "NitroFS extracted from your own Platinum ROM (the game does it on first run)")

# --- generated headers ------------------------------------------------------
# Everything lands in ${PPL_GENERATED_DIR}, laid out like the meson build
# directory: generated/*.h, res/.../*.naix, res/text/bank/*.h, nitro/fx/fx_const.h.
add_custom_command(
    OUTPUT ${PPL_GENERATED_DIR}/headers.stamp
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_headers.py
            ${PPL_GENERATED_DIR} --rom-dir ${PPL_ROM_DATA_DIR}
            --hosttools ${PPL_HOSTTOOLS_DIR}
            --stamp ${PPL_GENERATED_DIR}/headers.stamp
    DEPENDS ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_headers.py ${_ppl_metang_txt}
            ${PPL_DATAPROC_TOOLS}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating headers (metang, text banks, NARC indices, data tables)"
    VERBATIM)

add_custom_command(
    OUTPUT ${PPL_GENERATED_DIR}/pc_overlay_ids.h ${PPL_GENERATED_DIR}/pc_overlay_ids.c
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_overlay_ids.py
            ${PPL_GENERATED_DIR}/pc_overlay_ids.h
    DEPENDS ${PROJECT_SOURCE_DIR}/platinum.us/main.lsf.in ${PROJECT_SOURCE_DIR}/tools/pc_port/gen_overlay_ids.py
    COMMENT "Generating overlay IDs from main.lsf.in"
    VERBATIM)

add_custom_target(ppl_generated_headers DEPENDS
    ${PPL_GENERATED_DIR}/headers.stamp
    ${PPL_GENERATED_DIR}/pc_overlay_ids.h)

# English layer (language choice): the repository's English text banks and
# prebuilt files, next to the data extracted from the player's ROM
# (tools/pc_port/build_lang_en.py). Needs the ROM's data (first run of the game).
set(PPL_LANG_EN_DIR ${PROJECT_SOURCE_DIR}/pc_data/en)
if(EXISTS ${PPL_ROM_DATA_DIR}/msgdata/pl_msg.narc)
    file(GLOB _ppl_text_json CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/res/text/*.json)
    add_custom_command(
        OUTPUT ${PPL_LANG_EN_DIR}/.built
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pc_port/build_lang_en.py
                --template ${PPL_ROM_DATA_DIR} --msgenc $<TARGET_FILE:ppl_tool_msgenc>
                --generated ${PPL_GENERATED_DIR} --out ${PPL_LANG_EN_DIR}
                --stamp ${PPL_LANG_EN_DIR}/.built
        DEPENDS ${PPL_GENERATED_DIR}/headers.stamp ppl_tool_msgenc ${_ppl_text_json}
                ${PROJECT_SOURCE_DIR}/tools/pc_port/build_lang_en.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "English data layer -> pc_data/en"
        VERBATIM)
    add_custom_target(ppl_lang_en ALL DEPENDS ${PPL_LANG_EN_DIR}/.built)
else()
    message(STATUS "No extracted ROM in ${PPL_ROM_DATA_DIR}: English layer not built (run the game once, then rebuild)")
endif()

# --- flags ----------------------------------------------------------------
add_library(ppl_game_flags INTERFACE)
add_dependencies(ppl_game_flags ppl_generated_headers)

# Same order as the DS build: the game (include, asm, build root), its
# libraries, then the Nintendo SDKs. src/platform_pc/sdk_override holds the
# few SDK headers the PC needs changed (copies, edits under PLATFORM_PC).
target_include_directories(ppl_game_flags SYSTEM INTERFACE
    ${PROJECT_SOURCE_DIR}/src/platform_pc/nitro_compat
    ${PROJECT_SOURCE_DIR}/src/platform_pc/sdk_override
    # NitroDWC headers include each other without their folder (<nonport.h>).
    ${PROJECT_SOURCE_DIR}/src/platform_pc/sdk_override/gs
    ${PROJECT_SOURCE_DIR}/src/platform_pc/sdk_override/base
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/asm
    ${PPL_GENERATED_DIR}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/lib/crypto/include
    ${PROJECT_SOURCE_DIR}/lib/gds/include
    ${PROJECT_SOURCE_DIR}/lib/spl/include
    ${PROJECT_SOURCE_DIR}/lib/ppwlobby/include
    # meson adds the folder of every generated header given as a source:
    # some files include them without their path ("time_of_day.h").
    ${PPL_GENERATED_DIR}/generated
    ${PPL_GENERATED_DIR}/res/graphics/end_credits   # "ending.naix"
    ${PPL_SDK}/include
    ${PPL_NNS}/include
    ${PPL_DWC}/include ${PPL_DWC}/include/gs ${PPL_DWC}/include/base
    ${PPL_WIFI}/include
    ${PPL_VCT}/include)

target_compile_definitions(ppl_game_flags INTERFACE
    PLATFORM_PC=1
    POKEPLATINUM_GENERATED_ENUM
    POKEPLATINUM_REVISION=${PPL_REVISION}
    PM_KEEP_ASSERTS
    GAME_VERSION=VERSION_PLATINUM
    GAME_LANGUAGE=gPcGameLanguage   # chosen at run time (src/platform_pc/nitro/nitro_pc.c)
    _MSL_RESTRICT=
    SDK_TS SDK_4M SDK_ARM9 SDK_FINALROM SDK_CODE_ARM NNS_FINALROM
    _NITRO              # NitroDWC (GameSpy) platform, given to the game by the DWC dependency
    LINK_PPWLOBBY)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ppl_game_flags INTERFACE
        -include platform_pc_prefix.h
        -fsigned-char                 # MWCC: -char signed
        -fshort-wchar                 # DS wchar_t is 16-bit (UTF-16 text, NitroSystem fonts)
        -fno-optimize-sibling-calls   # complete backtraces for asserts and crashes
        -fms-extensions
        -fno-strict-aliasing          # DS code type-puns freely
        -fwrapv                       # and relies on 2's complement wrap-around
        -Wno-unknown-pragmas          # #pragma force_active/opt_*/unused
        -Wno-multichar
        # Accepted by Metrowerks, errors by default in recent Clang; harmless
        # here (NULL given as an integer, loosely typed callbacks, implicit int).
        -Wno-error=int-conversion
        -Wno-error=incompatible-pointer-types
        -Wno-error=incompatible-function-pointer-types
        -Wno-error=implicit-int
        # Implicit declarations are tolerated (as by Metrowerks) only after
        # checking that none returns a pointer: tools/pc_port/implicit_decls.py.
        -Wno-error=implicit-function-declaration)
    if(NOT PPL_WARN_PTR_CASTS)
        target_compile_options(ppl_game_flags INTERFACE
            -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
    endif()
endif()

# --- low-address executables ---------------------------------------------
function(ppl_low_address_link target)
    if(WIN32 AND (MINGW OR CMAKE_C_COMPILER_ID MATCHES "GNU|Clang") AND NOT MSVC)
        target_link_options(${target} PRIVATE
            -Wl,--image-base=0x10000000
            -Wl,--disable-dynamicbase
            -Wl,--disable-high-entropy-va)
    elseif(MSVC)
        target_link_options(${target} PRIVATE /BASE:0x10000000 /DYNAMICBASE:NO /HIGHENTROPYVA:NO)
    elseif(APPLE)
        # Default __PAGEZERO covers the whole low 4 GiB on 64-bit macOS.
        target_link_options(${target} PRIVATE -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x10000000 -Wl,-no_pie
            -Wl,-no_fixup_chains)   # chained fixups ignore the fixed image base
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    else()
        # Linux / BSD: a non-PIE executable placed at 0x10000000, above the
        # DS memory map (0x01FF0000-0x07010000) and below 4 GiB, like on
        # Windows (the game stores code and data addresses in 32-bit words).
        set(PPL_LD_TTEXT_SEGMENT ON)
        if(NOT CMAKE_VERSION VERSION_LESS 3.18)
            include(CheckLinkerFlag)
            check_linker_flag(C "-Wl,-Ttext-segment=0x10000000" PPL_LD_TTEXT_SEGMENT)
        endif()
        target_link_options(${target} PRIVATE -no-pie)
        if(PPL_LD_TTEXT_SEGMENT)
            target_link_options(${target} PRIVATE -Wl,-Ttext-segment=0x10000000)
        else()
            target_link_options(${target} PRIVATE -Wl,--image-base=0x10000000)
        endif()
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    endif()
endfunction()
