# Provides:
#   ppl::SDL2      SDL2 itself (link this from libraries)
#   ppl::SDL2main  SDL2main, only for executables whose main() is in a file
#                  including <SDL.h> (it renames main to SDL_main on Windows)
#
# 1. Uses an installed SDL2 (package manager, vcpkg, SDL2_DIR...) if found.
# 2. Otherwise, when PPL_FETCH_SDL2=ON (default), downloads the official
#    SDL2 release source and builds it statically alongside the game.

option(PPL_FETCH_SDL2 "Download and build SDL2 if it is not installed" ON)
set(PPL_SDL2_VERSION "2.30.9" CACHE STRING "SDL2 version fetched when not installed")

find_package(SDL2 CONFIG QUIET)

add_library(ppl_sdl2 INTERFACE)
add_library(ppl_sdl2main INTERFACE)

if(SDL2_FOUND)
    message(STATUS "SDL2: using installed package (${SDL2_VERSION})")
    if(TARGET SDL2::SDL2)
        target_link_libraries(ppl_sdl2 INTERFACE SDL2::SDL2)
    else()
        target_link_libraries(ppl_sdl2 INTERFACE SDL2::SDL2-static)
    endif()
elseif(PPL_FETCH_SDL2)
    message(STATUS "SDL2: not installed, fetching SDL ${PPL_SDL2_VERSION} source")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${PPL_SDL2_VERSION}/SDL2-${PPL_SDL2_VERSION}.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL2)
    target_link_libraries(ppl_sdl2 INTERFACE SDL2::SDL2-static)
else()
    message(FATAL_ERROR "SDL2 not found. Install it (e.g. apt install libsdl2-dev, "
                        "brew install sdl2, vcpkg install sdl2) or set PPL_FETCH_SDL2=ON.")
endif()

if(TARGET SDL2::SDL2main)
    target_link_libraries(ppl_sdl2main INTERFACE SDL2::SDL2main)
endif()

add_library(ppl::SDL2 ALIAS ppl_sdl2)
add_library(ppl::SDL2main ALIAS ppl_sdl2main)
