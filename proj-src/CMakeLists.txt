cmake_minimum_required(VERSION 3.25)

cmake_policy(SET CMP0091 NEW)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(totk_recomp_builder
    VERSION 1.0.0
    LANGUAGES CXX
)

set(APP_VERSION "${PROJECT_VERSION}" CACHE STRING "Application version")
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/include/icon.ico" CACHE FILEPATH "Application icon (.ico)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# FindLibArchive exposes a single LibArchive_LIBRARY variable without _DEBUG/_RELEASE
# splits, causing multi-config generators on Windows to bind debug/lib/archive.lib
# into Release builds. Map per-configuration IMPORTED_LOCATION properties explicitly.
if(TARGET LibArchive::LibArchive AND LibArchive_LIBRARY)
    if(LibArchive_LIBRARY MATCHES "/debug/lib/")
        string(REPLACE "/debug/lib/" "/lib/" _libarchive_release "${LibArchive_LIBRARY}")
        if(EXISTS "${_libarchive_release}")
            set_target_properties(LibArchive::LibArchive PROPERTIES
                IMPORTED_LOCATION_RELEASE "${_libarchive_release}"
                IMPORTED_LOCATION_RELWITHDEBINFO "${_libarchive_release}"
                IMPORTED_LOCATION_MINSIZEREL "${_libarchive_release}"
                IMPORTED_LOCATION_DEBUG "${LibArchive_LIBRARY}"
            )
        endif()
    elseif(LibArchive_LIBRARY MATCHES "/lib/")
        string(REPLACE "/lib/" "/debug/lib/" _libarchive_debug "${LibArchive_LIBRARY}")
        if(EXISTS "${_libarchive_debug}")
            set_target_properties(LibArchive::LibArchive PROPERTIES
                IMPORTED_LOCATION_RELEASE "${LibArchive_LIBRARY}"
                IMPORTED_LOCATION_RELWITHDEBINFO "${LibArchive_LIBRARY}"
                IMPORTED_LOCATION_MINSIZEREL "${LibArchive_LIBRARY}"
                IMPORTED_LOCATION_DEBUG "${_libarchive_debug}"
            )
        endif()
    endif()
endif()

set(EMBED_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/auto_build_data.h"
)

add_custom_command(
    OUTPUT "${EMBED_OUTPUT}"

    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/auto-build.py"
        "${EMBED_OUTPUT}"

    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/auto-build.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.py"

    COMMENT "Embedding auto-build.py"

    VERBATIM
)

add_executable(totk-auto-build
    src/main.cpp
    src/download.cpp
    src/extraction.cpp
    src/embedded_script.cpp
    src/check_nsp.cpp
    "${EMBED_OUTPUT}"
)

if(WIN32)
    string(REGEX MATCH "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?(\\.([0-9]+))?" _ver_match "${APP_VERSION}")

    if(CMAKE_MATCH_1)
        set(APP_VERSION_MAJOR "${CMAKE_MATCH_1}")
    else()
        set(APP_VERSION_MAJOR "0")
    endif()

    if(CMAKE_MATCH_3)
        set(APP_VERSION_MINOR "${CMAKE_MATCH_3}")
    else()
        set(APP_VERSION_MINOR "0")
    endif()

    if(CMAKE_MATCH_5)
        set(APP_VERSION_PATCH "${CMAKE_MATCH_5}")
    else()
        set(APP_VERSION_PATCH "0")
    endif()

    if(CMAKE_MATCH_7)
        set(APP_VERSION_TWEAK "${CMAKE_MATCH_7}")
    else()
        set(APP_VERSION_TWEAK "0")
    endif()

    if(EXISTS "${APP_ICON}")
        file(TO_CMAKE_PATH "${APP_ICON}" APP_ICON_RC)
        set(APP_ICON_LINE "1 ICON \"${APP_ICON_RC}\"")
    else()
        set(APP_ICON_LINE "// No icon found at ${APP_ICON}")
    endif()

    set(RC_OUTPUT
        "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
    )

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
        "${RC_OUTPUT}"
        @ONLY
    )

    if(EXISTS "${APP_ICON}")
        set_property(SOURCE "${RC_OUTPUT}" APPEND PROPERTY OBJECT_DEPENDS "${APP_ICON}")
    endif()

    target_sources(totk-auto-build PRIVATE "${RC_OUTPUT}")
endif()

target_compile_definitions(totk-auto-build PRIVATE
    APP_VERSION="${APP_VERSION}"
)

target_include_directories(totk-auto-build PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(totk-auto-build PRIVATE
    CURL::libcurl
    LibArchive::LibArchive
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(totk-auto-build PRIVATE
        /W4
        /permissive-
        /utf-8
    )
else()
    target_compile_options(totk-auto-build PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_options(totk-auto-build PRIVATE
        -static-libgcc
        -static-libstdc++
    )
endif()