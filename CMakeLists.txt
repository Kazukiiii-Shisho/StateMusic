cmake_minimum_required(VERSION 3.20)
project(StateMusicAudio CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Provide these two paths (see README.md). miniaudio.h must sit next to Plugin.cpp.
#   REFRAMEWORK_DIR : REFramework source checkout (has include/reframework/API.hpp
#                     AND dependencies/lua/src = the exact Lua REFramework uses)
#   SOL2_DIR        : sol2 include dir (has sol/sol.hpp)
#
# IMPORTANT: REFramework uses vanilla Lua 5.4 (PUC-Rio), NOT LuaJIT. We compile its
# exact Lua source straight into the plugin so the C ABI matches REFramework's Lua.
# Linking a different Lua (e.g. LuaJIT) makes the game crash at load.
# ---------------------------------------------------------------------------
foreach(_v REFRAMEWORK_DIR SOL2_DIR)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "Set -D${_v}=C:/path/to/...")
    endif()
endforeach()

file(GLOB LUA_SOURCES "${REFRAMEWORK_DIR}/dependencies/lua/src/*.c")

# Ogg Vorbis. miniaudio decodes WAV/FLAC/MP3 by itself but needs stb_vorbis for .ogg -
# without it those files are listed by the mod and then play silently. Plugin.cpp includes
# only stb_vorbis's declarations (STB_VORBIS_HEADER_ONLY); the implementation is compiled
# HERE as C, because pulling it into the C++ unit breaks the Windows headers. It still
# links into the same single DLL - no extra plugin file.
add_library(StateMusicAudio SHARED Plugin.cpp stb_vorbis.c ${LUA_SOURCES})

target_compile_definitions(StateMusicAudio PRIVATE _CRT_SECURE_NO_WARNINGS)

target_include_directories(StateMusicAudio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}               # miniaudio.h
    ${REFRAMEWORK_DIR}/include                # reframework/API.hpp
    ${SOL2_DIR}                               # sol/sol.hpp
    ${REFRAMEWORK_DIR}/dependencies/lua/src   # lua.h / lua.hpp (vanilla Lua 5.4)
)

if(WIN32)
    target_link_libraries(StateMusicAudio PRIVATE ole32 winmm)
endif()

set_target_properties(StateMusicAudio PROPERTIES
    OUTPUT_NAME "StateMusicAudio"
    PREFIX ""
    SUFFIX ".dll"
)
