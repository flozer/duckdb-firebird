cmake_minimum_required(VERSION 3.5)

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

# When built inside the DuckDB extension build system the parent CMake defines
# both targets via build_static_extension / build_loadable_extension. If we are
# built standalone (for local development), fall back to a single shared lib.
if(NOT DEFINED DUCKDB_EXTENSION_NAME)
    add_library(${LOADABLE_EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})
    add_library(${EXTENSION_NAME} STATIC ${EXTENSION_SOURCES})
else()
    build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
    build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
endif()

# libfbclient (Firebird client library).
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FBCLIENT IMPORTED_TARGET fbclient)
endif()

if(FBCLIENT_FOUND)
    target_include_directories(${EXTENSION_NAME}         PRIVATE ${FBCLIENT_INCLUDE_DIRS})
    target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${FBCLIENT_INCLUDE_DIRS})
    target_link_libraries(${EXTENSION_NAME}              PUBLIC  ${FBCLIENT_LIBRARIES})
    target_link_libraries(${LOADABLE_EXTENSION_NAME}     PUBLIC  ${FBCLIENT_LIBRARIES})
else()
    # Fallback: assume fbclient is on the default include/lib path.
    target_link_libraries(${EXTENSION_NAME}          PUBLIC fbclient)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} PUBLIC fbclient)
endif()

install(TARGETS ${EXTENSION_NAME}
    EXPORT  "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
