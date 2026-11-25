# Sources of the game and of the Nintendo libraries that are compiled for the
# PC host, and those that the PC layer (src/platform_pc) replaces.
#
# Defines PPL_GAME_SOURCES.

set(_root ${PROJECT_SOURCE_DIR})

# --- the game: exactly the C files platinum.us/main.lsf.in links -------------
file(STRINGS ${_root}/platinum.us/main.lsf.in _lsf_objects REGEX "^[ \t]*Object[ \t]+src/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_root}/platinum.us/main.lsf.in)
set(_game "")
foreach(_line IN LISTS _lsf_objects)
    string(REGEX REPLACE "^[ \t]*Object[ \t]+(src/[^ \t(]+)\\.(c|o).*$" "\\1" _stem "${_line}")
    if(EXISTS ${_root}/${_stem}.c)
        list(APPEND _game ${_root}/${_stem}.c)
    endif()
endforeach()
list(REMOVE_DUPLICATES _game)
# The DS build compiles every game source with include/pch/global_pch.h as a
# precompiled header (meson.build: c_pch): the sources rely on it.
set_property(SOURCE ${_game} APPEND PROPERTY COMPILE_OPTIONS "-include${_root}/include/pch/global_pch.h")
set(PPL_GAME_PCH_SOURCES ${_game})

# --- libraries of the repository ---------------------------------------------
# lib/spl: particle system (plain C). lib/gds: Global Data Station (online,
# overlay 61; C). lib/crypto: RC4 is ARM assembly (online only, excluded),
# the rest is C. lib/ppwlobby: Wi-Fi Plaza lobby in C++ (online): stubs.
file(GLOB _libspl CONFIGURE_DEPENDS ${_root}/lib/spl/src/*.c)
file(GLOB _libgds CONFIGURE_DEPENDS ${_root}/lib/gds/src/*.c)
file(GLOB _libcrypto CONFIGURE_DEPENDS ${_root}/lib/crypto/src/*.c)
list(FILTER _libcrypto EXCLUDE REGEX ".*/rc4_init\\.c$")

# --- NitroSystem: plain C, compiled unchanged ------------------------------
set(_nns ${PPL_NNS}/libraries)
file(GLOB_RECURSE _nnsys CONFIGURE_DEPENDS
    ${_nns}/g2d/src/*.c ${_nns}/g3d/src/*.c ${_nns}/gfd/src/*.c ${_nns}/snd/src/*.c)
# fnd is part of the Nitro layer library (src/platform_pc/CMakeLists.txt).
# Debug printers and modelling-tool helpers are not linked by the game.
list(FILTER _nnsys EXCLUDE REGEX ".*/g3d/src/binres/res_print.*\\.c$")

# --- NitroSDK ----------------------------------------------------------------
set(_sdk ${PPL_SDK}/libraries)
file(GLOB _sdk_fx   CONFIGURE_DEPENDS ${_sdk}/fx/src/*.c)
file(GLOB _sdk_gx   CONFIGURE_DEPENDS ${_sdk}/gx/src/*.c)
file(GLOB _sdk_math CONFIGURE_DEPENDS ${_sdk}/math/src/*.c)
file(GLOB _sdk_mi   CONFIGURE_DEPENDS ${_sdk}/mi/src/*.c)
file(GLOB _sdk_std  CONFIGURE_DEPENDS ${_sdk}/std/src/*.c)
set(PPL_SDK_SOURCES ${_sdk_fx} ${_sdk_gx} ${_sdk_math} ${_sdk_mi} ${_sdk_std}
    ${_sdk}/snd/src/snd_util.c)   # pure tables and calculations
# Hand-written ARM assembly or hardware access, replaced by the PC Nitro layer
# (src/platform_pc/nitro: sdk_asm_pc.c, mi_nitro_pc.c, mi_dma_pc.c).
list(FILTER PPL_SDK_SOURCES EXCLUDE REGEX
    ".*/(gxasm|mi_memory|mi_swap|mi_uncompress|mi_dma|mi_dma_card|mi_dma_gxcommand|mi_dma_hblank|mi_dma_mainmem|mi_dma_vblank|mi_init|mi_card|mi_wram|mi_cache|mi_exMemory|qsort)\\.c$")
# Not built (replaced by the PC Nitro layer or meaningless on PC):
#   os/*  (kernel: threads, IRQ, cache, arenas...)   -> nitro/os_nitro_pc.c
#   fs/*  (NitroFS, overlays)                        -> nitro/fs_nitro_pc.c
#   card/*, ctrdg/*, pxi/*, mb/*, wm*, wvr, spi, pad, rtc (but convert.c)
#   snd/* (ARM9 side of the ARM7 sound driver)       -> nitro/snd_pc.c
#   dgt (prebuilt ARM library)                       -> nitro/dgt_pc.c
# Files mixing C and ARM `asm` functions: compiled from copies without the
# asm (src/platform_pc/sdk_src, tools/pc_port/make_sdk_override.py).
set(_sdk_src_pc ${PROJECT_SOURCE_DIR}/src/platform_pc/sdk_src)
file(GLOB_RECURSE _sdk_stripped CONFIGURE_DEPENDS RELATIVE ${_sdk_src_pc}/NitroSDK ${_sdk_src_pc}/NitroSDK/*.c)
foreach(_rel IN LISTS _sdk_stripped)
    list(REMOVE_ITEM PPL_SDK_SOURCES ${_sdk}/${_rel})
    list(APPEND PPL_SDK_SOURCES ${_sdk_src_pc}/NitroSDK/${_rel})
endforeach()
file(GLOB_RECURSE _nns_patched CONFIGURE_DEPENDS RELATIVE ${_sdk_src_pc}/NitroSystem ${_sdk_src_pc}/NitroSystem/*.c)
foreach(_rel IN LISTS _nns_patched)
    list(REMOVE_ITEM _nnsys ${_nns}/${_rel})
    list(APPEND _nnsys ${_sdk_src_pc}/NitroSystem/${_rel})
endforeach()

# Each Nintendo library compiles with its own private include folder
# (libraries/<lib>/include), as in its meson.build.
function(ppl_library_private_includes)
    foreach(_src IN LISTS ARGN)
        if(_src MATCHES "/(NitroSDK-[^/]+|NitroSystem-[^/]+)/libraries/([^/]+)/")
            set(_sub ${CMAKE_MATCH_1})
            set(_lib ${CMAKE_MATCH_2})
        elseif(_src MATCHES "/sdk_src/(NitroSDK|NitroSystem)/([^/]+)/")
            set(_lib ${CMAKE_MATCH_2})
            if(CMAKE_MATCH_1 STREQUAL "NitroSDK")
                get_filename_component(_sub ${PPL_SDK} NAME)
            else()
                get_filename_component(_sub ${PPL_NNS} NAME)
            endif()
        else()
            continue()
        endif()
        set(_dir ${PPL_SUBPROJECTS}/${_sub}/libraries/${_lib}/include)
        if(EXISTS ${_dir})
            set_property(SOURCE ${_src} APPEND PROPERTY INCLUDE_DIRECTORIES ${_dir})
        endif()
    endforeach()
endfunction()
ppl_library_private_includes(${_nnsys} ${PPL_SDK_SOURCES} ${PPL_NITRO_SDK_SOURCES})
# libraries/gx includes its private gxasm.h by relative path ("../include/"):
# the corrected copy is force-included first, its include guard wins.
foreach(_src IN LISTS PPL_SDK_SOURCES)
    if(_src MATCHES "/gx/src/[^/]+\\.c$")
        set_property(SOURCE ${_src} APPEND PROPERTY COMPILE_OPTIONS
            "-include${PROJECT_SOURCE_DIR}/src/platform_pc/sdk_override/gxasm.h")
    endif()
endforeach()

set(PPL_GAME_SOURCES ${_game} ${_libspl} ${_libgds} ${_libcrypto} ${_nnsys} ${PPL_SDK_SOURCES})
list(LENGTH _game _n_game)
list(LENGTH PPL_GAME_SOURCES _n_all)
message(STATUS "PC game sources: ${_n_game} game files, ${_n_all} total")
