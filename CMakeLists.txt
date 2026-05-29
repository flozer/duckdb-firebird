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
    src/firebird_client_loader.cpp
    src/firebird_types.cpp
    src/firebird_query.cpp
    src/firebird_scanner.cpp
    src/firebird_storage.cpp
    src/firebird_observability.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# --- libfbclient ------------------------------------------------------------
#
# We do NOT link against libfbclient. The legacy ISC entry-points are
# resolved at runtime by src/firebird_client_loader.cpp via dlopen /
# LoadLibrary, so the build only needs the Firebird header tree at
# compile time. The headers ship with the extension under
# `third_party/firebird/include/`, which lets the community-extensions
# CI build the extension on platforms where libfbclient is not
# installed (or available via vcpkg).
#
# Header resolution order:
#   1. FB_SDK_ROOT (cmake or env var) - point at an unpacked Firebird
#      install. Developers can opt into the upstream FB5 SDK headers
#      this way.
#   2. Vendored copy under `third_party/firebird/include/` - the
#      default. Always present; required for the community CI path.
#
# On Linux/macOS we additionally pass `-ldl` so the runtime loader
# can call dlopen/dlsym (already standard on most distros, but
# explicit linkage avoids surprises in slim build images).

if(NOT DEFINED FB_SDK_ROOT AND DEFINED ENV{FB_SDK_ROOT})
    set(FB_SDK_ROOT $ENV{FB_SDK_ROOT})
endif()

set(FB_INCLUDE_DIR "")

if(FB_SDK_ROOT)
    set(FB_INCLUDE_CANDIDATES
        "${FB_SDK_ROOT}/include"
        "${FB_SDK_ROOT}/Include"
        "${FB_SDK_ROOT}")
    find_path(FB_INCLUDE_DIR ibase.h PATHS ${FB_INCLUDE_CANDIDATES} NO_DEFAULT_PATH)
    if(NOT FB_INCLUDE_DIR)
        message(WARNING
            "FB_SDK_ROOT=${FB_SDK_ROOT} did not yield ibase.h; "
            "falling back to vendored third_party/firebird headers.")
    else()
        message(STATUS "libfbclient headers via FB_SDK_ROOT=${FB_SDK_ROOT}")
        message(STATUS "  ibase.h:    ${FB_INCLUDE_DIR}")
    endif()
endif()

if(NOT FB_INCLUDE_DIR)
    set(FB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/firebird/include")
    message(STATUS "libfbclient headers via vendored copy: ${FB_INCLUDE_DIR}")
endif()

target_include_directories(${EXTENSION_NAME}          PRIVATE ${FB_INCLUDE_DIR})
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${FB_INCLUDE_DIR})

# Runtime dynamic-loader dependency (dlopen / dlsym). LoadLibrary on
# Windows is satisfied by kernel32 which CMake links by default.
if(UNIX AND NOT APPLE)
    target_link_libraries(${EXTENSION_NAME}          dl)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} dl)
endif()

install(TARGETS ${EXTENSION_NAME}
    EXPORT  "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
