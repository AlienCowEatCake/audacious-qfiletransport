cmake_minimum_required(VERSION 3.5)

project(qfiletransport VERSION 0.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(DEFINED ENV{AUDACIOUS_ROOT_DIR})
    set(AUDACIOUS_ROOT_DIR_DEFAULT "$ENV{AUDACIOUS_ROOT_DIR}")
else()
    set(AUDACIOUS_ROOT_DIR_DEFAULT "$ENV{ProgramFiles\(x86\)}\\Audacious")
endif()
set(AUDACIOUS_ROOT_DIR "${AUDACIOUS_ROOT_DIR_DEFAULT}" CACHE PATH "Path to Audacions binaries root directory")

set(AUDCORE_DLL_NAME)
foreach(AUDCORE_DLL_NAME_TEST "audcore" "audcore5")
    if(EXISTS "${AUDACIOUS_ROOT_DIR}/bin/${AUDCORE_DLL_NAME_TEST}.dll")
        set(AUDCORE_DLL_NAME "${AUDCORE_DLL_NAME_TEST}")
        break()
    endif()
endforeach()
if(NOT AUDCORE_DLL_NAME)
    message(FATAL_ERROR "AUDCORE was not found, please check AUDACIOUS_ROOT_DIR=\"${AUDACIOUS_ROOT_DIR}\"")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/libaudcore.dll.a")
    if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${AUDCORE_DLL_NAME}.def")
        message("Generating ${AUDCORE_DLL_NAME}.def ...")
        find_program(GENDEF_PROG gendef REQUIRED)
        execute_process(
            COMMAND "${GENDEF_PROG}" "${AUDACIOUS_ROOT_DIR}/bin/${AUDCORE_DLL_NAME}.dll"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )

        message("Patching ${AUDCORE_DLL_NAME}.def ...")
        file(READ "${CMAKE_CURRENT_BINARY_DIR}/${AUDCORE_DLL_NAME}.def" AUDCORE_DEF_DATA)
        string(REGEX REPLACE "@[0-9]*" "" AUDCORE_DEF_DATA "${AUDCORE_DEF_DATA}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${AUDCORE_DLL_NAME}.def" "${AUDCORE_DEF_DATA}")
    endif()

    message("Generating libaudcore.dll.a ...")
    find_program(DLLTOOL_PROG dlltool REQUIRED)
    execute_process(
        COMMAND "${DLLTOOL_PROG}" -d "${AUDCORE_DLL_NAME}.def" -D "${AUDCORE_DLL_NAME}.dll" -l "libaudcore.dll.a"
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
include_directories(SYSTEM "${CMAKE_CURRENT_LIST_DIR}/audacious/src" "${CMAKE_CURRENT_BINARY_DIR}")

set(PLUGIN_VERSION "v${CMAKE_PROJECT_VERSION}")
find_package(Git)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE GIT_REVISION
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    message("Git revision: ${GIT_REVISION}")
    set(PLUGIN_VERSION "${PLUGIN_VERSION} \(${GIT_REVISION}\)")
endif()

find_package(Qt5 REQUIRED COMPONENTS Core)

add_library(${CMAKE_PROJECT_NAME} SHARED qfiletransport.cpp)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE audcore Qt5::Core)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINK_FLAGS_RELEASE -s)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "AUDCORE_DLL_NAME=\"${AUDCORE_DLL_NAME}\"")
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "PLUGIN_VERSION=\"${PLUGIN_VERSION}\"")
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "EXPORT=__declspec(dllexport)")
