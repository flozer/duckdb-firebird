cmake_minimum_required(VERSION 3.5)

# This CMakeLists.txt is intended to be included from DuckDB's extension build
# system via extension_config.cmake / duckdb_extension_load. Direct standalone
# invocation will not produce a working .duckdb_extension — use `make release`.

set(TARGET_NAME firebird)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/firebird_extension.cpp
    src/firebird_client.cpp
    src/firebird_types.cpp
    src/firebird_query.cpp
    src/firebird_scanner.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# --- libfbclient ------------------------------------------------------------
#
# Resolution order:
#   1. FB_SDK_ROOT (cmake or env var) — point at an unpacked Firebird
#      install. Used by CI on Windows + macOS where libfbclient isn't in
#      vcpkg and isn't shipped by the OS package manager.
#   2. pkg-config (`fbclient`) — Linux distro packages.
#   3. Bare `-lfbclient` — assume default include/lib search paths.

if(NOT DEFINED FB_SDK_ROOT AND DEFINED ENV{FB_SDK_ROOT})
    set(FB_SDK_ROOT $ENV{FB_SDK_ROOT})
endif()

if(FB_SDK_ROOT)
    message(STATUS "libfbclient via FB_SDK_ROOT=${FB_SDK_ROOT}")

    set(FB_INCLUDE_CANDIDATES
        "${FB_SDK_ROOT}/include"
        "${FB_SDK_ROOT}/Include"
        "${FB_SDK_ROOT}")
    set(FB_LIB_CANDIDATES
        "${FB_SDK_ROOT}/lib"
        "${FB_SDK_ROOT}/Lib"
        "${FB_SDK_ROOT}")

    find_path(FB_INCLUDE_DIR ibase.h PATHS ${FB_INCLUDE_CANDIDATES} NO_DEFAULT_PATH)

    if(WIN32)
        find_library(FB_LIBRARY
            NAMES fbclient_ms fbclient
            PATHS ${FB_LIB_CANDIDATES}
            NO_DEFAULT_PATH)
    else()
        find_library(FB_LIBRARY
            NAMES fbclient
            PATHS ${FB_LIB_CANDIDATES}
            NO_DEFAULT_PATH)
    endif()

    if(NOT FB_INCLUDE_DIR OR NOT FB_LIBRARY)
        message(FATAL_ERROR
            "FB_SDK_ROOT=${FB_SDK_ROOT} did not yield ibase.h and a fbclient "
            "import library. Looked for headers in ${FB_INCLUDE_CANDIDATES} "
            "and libraries in ${FB_LIB_CANDIDATES}.")
    endif()

    message(STATUS "  ibase.h:    ${FB_INCLUDE_DIR}")
    message(STATUS "  fbclient:   ${FB_LIBRARY}")

    target_include_directories(${EXTENSION_NAME}          PRIVATE ${FB_INCLUDE_DIR})
    target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${FB_INCLUDE_DIR})
    target_link_libraries(${EXTENSION_NAME}          ${FB_LIBRARY})
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ${FB_LIBRARY})
else()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(FBCLIENT IMPORTED_TARGET fbclient)
    endif()

    if(FBCLIENT_FOUND)
        target_include_directories(${EXTENSION_NAME}          PRIVATE ${FBCLIENT_INCLUDE_DIRS})
        target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${FBCLIENT_INCLUDE_DIRS})
        target_link_libraries(${EXTENSION_NAME}          ${FBCLIENT_LIBRARIES})
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${FBCLIENT_LIBRARIES})
    else()
        # Last resort: rely on default search paths (Linux with /usr/lib).
        target_link_libraries(${EXTENSION_NAME}          fbclient)
        target_link_libraries(${LOADABLE_EXTENSION_NAME} fbclient)
    endif()
endif()

install(TARGETS ${EXTENSION_NAME}
    EXPORT  "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
