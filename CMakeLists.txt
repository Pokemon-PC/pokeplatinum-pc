cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Pokemon Platinum - native PC port (CMake entry point)
#
# Builds the game for Windows / Linux / macOS (x86_64) with Clang, SDL2 and
# OpenGL. The matching Nintendo DS ROM is NOT built by CMake: keep using
# `make` / meson exactly as described in INSTALL.md. Nothing in the PC port
# changes the DS build (PC code lives in src/platform_pc and tools/pc_port,
# or under #ifdef PLATFORM_PC).
#
# See PORTING_ROADMAP.md.
# ---------------------------------------------------------------------------

project(PokePlatinumPC
    VERSION 0.1.0
    DESCRIPTION "Native PC port of the pokeplatinum decompilation"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "PC port is developed for 64-bit hosts; a 32-bit build is untested.")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(PplSDL2)
include(PplGame)

enable_testing()

find_package(OpenGL REQUIRED)

add_subdirectory(src/platform_pc)
