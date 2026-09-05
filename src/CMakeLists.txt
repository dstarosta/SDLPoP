cmake_minimum_required(VERSION 3.14) # for FetchContent_MakeAvailable
project(SDLPoP)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if(NOT MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=c99 -D_GNU_SOURCE=1")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")
endif()

# have CMake output binaries to the directory that contains the source files
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SDLPoP_SOURCE_DIR}/..")
# Multi-config generators would otherwise append the config name (../Debug/prince.exe).
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_cfg}" _cfg_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper} "${SDLPoP_SOURCE_DIR}/..")
endforeach()

# windres mangles -I paths containing spaces, so let gcc preprocess the .rc first.
# Must precede FetchContent so it also covers SDL's own version.rc.
if(WIN32 AND CMAKE_RC_COMPILER MATCHES "windres")
    set(CMAKE_RC_COMPILE_OBJECT
        "<CMAKE_C_COMPILER> -E -xc -DRC_INVOKED <DEFINES> <INCLUDES> <SOURCE> -o <OBJECT>.pp.rc"
        "<CMAKE_RC_COMPILER> <FLAGS> -i <OBJECT>.pp.rc -o <OBJECT>")
endif()

if(WIN32 AND NOT MSVC)
    # Use the -mwindows compiler flag when compiling with MinGW to hide the console window
    # Only do this when not in debug mode
    if (NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mwindows")
    endif (NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
endif()

if(PSP)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
endif()

# Use an installed SDL3 if there is one, otherwise download and build it.
# -DSDL3_ROOT=<dir> picks a specific install; -DSDLPoP_SYSTEM_SDL3=ON forbids downloading.
option(SDLPoP_SYSTEM_SDL3 "Require a preinstalled SDL3; never fetch from source" OFF)

if(DEFINED SDL3 AND NOT DEFINED SDL3_ROOT)
    set(SDL3_ROOT "${SDL3}")
endif()

find_package(SDL3 QUIET CONFIG)

if(NOT SDL3_FOUND)
    if(SDLPoP_SYSTEM_SDL3)
        message(FATAL_ERROR
            "SDL3 not found and SDLPoP_SYSTEM_SDL3=ON. Install SDL3 (dev package, "
            "vcpkg, SDL3_ROOT, or Homebrew) or drop the flag to build SDL3 from source.")
    endif()

    message(STATUS "SDL3 not found on the system -- fetching and building it from source.")
    include(FetchContent)

    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.16
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

set(SOURCE_FILES
        main.c
        common.h
        config.h
        data.c
        data.h
        proto.h
        types.h
        seg000.c
        seg001.c
        seg002.c
        seg003.c
        seg004.c
        seg005.c
        seg006.c
        seg007.c
        seg008.c
        seg009.c
        seqtbl.c
        options.c
        midi.c opl3.c opl3.h mt32synth.c mt32synth.h
        replay.c
        lighting.c
        screenshot.c
        menu.c
        stb_vorbis.c
        icon.rc
        )

if (NOT(APPLE AND CREATE_BUNDLE))
    add_executable(prince ${SOURCE_FILES})
    if(MSVC)
        # MSVC equivalent of MinGW's -mwindows. Per-config, since the Visual Studio
        # generator builds every config from one CMAKE_BUILD_TYPE-less configure.
        set_target_properties(prince PROPERTIES
                WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
    endif()
else()

    # macOS-specific:
    # By default, a normal binary executable will be created.
    # If you want an Application Bundle instead, pass CREATE_BUNDLE to CMake like so: -DCREATE_BUNDLE=1

    # NOTE: Currently, the SDL3 framework is not being correctly included/linked in the bundle!
    #       As long as this isn't fixed, these bundles will only work if SDL3 is installed.

    set (SDLPoP_VERSION "1.24 RC")
    set (MACOSX_BUNDLE_INFO_STRING          ${PROJECT_NAME})
    set (MACOSX_BUNDLE_ICON_FILE            "icon.icns")
    set_source_files_properties(${MACOSX_BUNDLE_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set (MACOSX_BUNDLE_GUI_IDENTIFIER       "org.princed.SDLPoP")
    set (MACOSX_BUNDLE_LONG_VERSION_STRING  ${SDLPoP_VERSION})
    set (MACOSX_BUNDLE_BUNDLE_NAME          "SDLPoP")
    set (MACOSX_BUNDLE_SHORT_VERSION_STRING ${SDLPoP_VERSION})
    set (MACOSX_BUNDLE_BUNDLE_VERSION       ${SDLPoP_VERSION})
    set (MACOSX_BUNDLE_COPYRIGHT            "GNU General Public Licence, v3")
    add_executable(prince MACOSX_BUNDLE ${SOURCE_FILES} ${MACOSX_BUNDLE_ICON_FILE})
    add_custom_command(TARGET prince POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/../data ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/prince.app/Contents/MacOS/data)
    add_custom_command(TARGET prince POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/../doc ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/prince.app/Contents/MacOS/doc)
    add_custom_command(TARGET prince POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/../SDLPoP.ini ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/prince.app/Contents/MacOS/SDLPoP.ini)
endif()

# SDL3main provides the WinMain -> SDL_main shim; only exists on some platforms.
if(TARGET SDL3::SDL3main)
    target_link_libraries(prince SDL3::SDL3main)
endif()

# mt32synth loads libmt32emu dynamically (dlopen/LoadLibrary). ${CMAKE_DL_LIBS} is the
# portable dynamic-loader library: empty on Windows, "dl" on Linux/*BSD.
if(WIN32)
    target_link_libraries(prince SDL3::SDL3 ${CMAKE_DL_LIBS})
else()
    target_link_libraries(prince SDL3::SDL3 m ${CMAKE_DL_LIBS})
endif()

# A from-source SDL3 isn't installed anywhere, so put it next to the executable.
if(TARGET SDL3::SDL3-shared AND SDL_SHARED)
    if(WIN32)
        set(_sdl3_runtime_file $<TARGET_FILE:SDL3::SDL3-shared>)
    else()
        # The loader looks up the SONAME (libSDL3.so.0), not the symlink TARGET_FILE gives.
        set(_sdl3_runtime_file $<TARGET_SONAME_FILE:SDL3::SDL3-shared>)
    endif()
    add_custom_command(TARGET prince POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_sdl3_runtime_file} $<TARGET_FILE_DIR:prince>
        COMMENT "Copying SDL3 shared library next to prince")
endif()

if(PSP)
    create_pbp_file(
        TARGET prince
        ICON_PATH ${CMAKE_SOURCE_DIR}/iconPSP.png
        BACKGROUND_PATH NULL
        PREVIEW_PATH NULL
        TITLE prince
    )
endif()
