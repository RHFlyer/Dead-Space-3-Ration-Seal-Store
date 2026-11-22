# Builds the Ration Seal Store add-on for the DS3 Mod Loader:
#     cmake -S . -B build -A Win32
#     cmake --build build --config Release
# The mod is then ready to copy, as a folder, into the game's plugins folder: build\plugins\SealStore\.
# scripts\package-mod.ps1 makes the release zip from it.

cmake_minimum_required(VERSION 3.20)
project(ration_seal_store C CXX)

# Dead Space 3 is a 32-bit game, so mods must be 32-bit DLLs too. "-A Win32" picks that.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "deadspace3.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The same build rules as the DS3 Mod Loader's SDK samples (its sdk/CMakeLists.txt, ds3_mod()).
add_library(SealStore SHARED SealStore/seal_store.cpp)
target_compile_definitions(SealStore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(SealStore PRIVATE /W4 /permissive-)
endif()
# .asi like most game mods; the static C runtime, so players need no Visual C++ redistributable.
set_target_properties(SealStore PROPERTIES
    SUFFIX ".asi"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# What ships: the DLL, mod.ini and CHANGELOG.txt. Never the player's settings file (seal_store.ini): copying the
# folder over an older version would reset it.
set(out "${CMAKE_BINARY_DIR}/plugins/SealStore")
add_custom_command(TARGET SealStore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${out}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SealStore>"
            "${CMAKE_CURRENT_SOURCE_DIR}/SealStore/mod.ini" "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.txt" "${out}"
    COMMENT "SealStore: ready in ${out}")
