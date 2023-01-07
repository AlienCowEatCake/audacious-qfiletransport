cmake_minimum_required(VERSION 3.5)

project(qfiletransport VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(AUDACIOUS_ROOT_DIR "$ENV{ProgramFiles\(x86\)}\\Audacious" CACHE PATH "Path to Audacions binaries root directory")

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/libaudcore.dll.a")
    if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/audcore.def")
        message("Generating audcore.def ...")
        find_program(GENDEF_PROG gendef REQUIRED)
        execute_process(
            COMMAND "${GENDEF_PROG}" "${AUDACIOUS_ROOT_DIR}/bin/audcore.dll"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )

        message("Patching audcore.def ...")
        file(READ "${CMAKE_CURRENT_BINARY_DIR}/audcore.def" AUDCORE_DEF_DATA)
        string(REGEX REPLACE "@[0-9]*" "" AUDCORE_DEF_DATA "${AUDCORE_DEF_DATA}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/audcore.def" "${AUDCORE_DEF_DATA}")
    endif()

    message("Generating libaudcore.dll.a ...")
    find_program(DLLTOOL_PROG dlltool REQUIRED)
    execute_process(
        COMMAND "${DLLTOOL_PROG}" -d "audcore.def" -D "audcore.dll" -l "libaudcore.dll.a"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()
add_library(audcore SHARED IMPORTED)
set_property(TARGET audcore PROPERTY IMPORTED_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/libaudcore.dll.a")

include(TestBigEndian)
test_big_endian(BIGENDIAN)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/audacious/src/libaudcore/audio.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libaudcore/audio.h"
    @ONLY
)
include_directories("${CMAKE_CURRENT_LIST_DIR}/audacious/src" "${CMAKE_CURRENT_BINARY_DIR}")

find_package(Qt5 REQUIRED COMPONENTS Core)

add_library(${CMAKE_PROJECT_NAME} SHARED qfiletransport.cpp)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt5::Core audcore)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINK_FLAGS_RELEASE -s)
