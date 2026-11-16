cmake_minimum_required(VERSION 3.21.0)
project(ReZeroDetour
    VERSION 0.1.0
    DESCRIPTION "A lightweight inline hook library for Windows x64"
    LANGUAGES CXX
)

# ----------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------
set(REZERODETOUR_DISASSEMBLER "ZYDIS" CACHE STRING "Disassembler backend")

set_property(CACHE REZERODETOUR_DISASSEMBLER PROPERTY STRINGS
    FADEC
    BDDISASM
    HDE64
    ZYDIS
)

option(REZERODETOUR_BUILD_TESTS "Build ReZeroDetour tests" OFF)

message(STATUS "ReZeroDetour disassembler backend ${REZERODETOUR_DISASSEMBLER}")
message(STATUS "Build the ReZeroDetour tests ${REZERODETOUR_BUILD_TESTS}")

# ----------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(REZERODETOUR_NO_EXCEPTIONS -fno-exceptions -fno-unwind-tables)
    set(REZERODETOUR_NO_RTTI -fno-rtti)
    set(REZERODETOUR_WARNING_OPTIONS -Wall -Wextra)
elseif(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(REZERODETOUR_NO_EXCEPTIONS /EHs- /EHc- /D_HAS_EXCEPTIONS=0)
    set(REZERODETOUR_NO_RTTI /GR-)
    set(REZERODETOUR_WARNING_OPTIONS /W4 /utf-8)
endif()

# ----------------------------------------------------------------------
# Library
# ----------------------------------------------------------------------
add_library(ReZeroDetour STATIC)


add_library(ReZero::ReZeroDetour ALIAS ReZeroDetour)


target_compile_features(ReZeroDetour
    PUBLIC  cxx_std_23
)

target_include_directories(ReZeroDetour
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(ReZeroDetour PUBLIC
    ${REZERODETOUR_NO_EXCEPTIONS}
    ${REZERODETOUR_NO_RTTI}
    ${REZERODETOUR_WARNING_OPTIONS}
)

# ----------------------------------------------------------------------
# Disassembler backends
# ----------------------------------------------------------------------
if(REZERODETOUR_DISASSEMBLER STREQUAL "FADEC")
    message(FATAL_ERROR "FADEC backend is not implemented yet.")
elseif(REZERODETOUR_DISASSEMBLER STREQUAL "ZYDIS")
    target_sources(ReZeroDetour PRIVATE
        src/Disassembler.cpp
    )
    include(FetchContent)

    set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)

    # Detour only needs instruction decoding.
    set(ZYDIS_FEATURE_DECODER ON CACHE BOOL "" FORCE)
    set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
    set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        Zydis
        GIT_REPOSITORY https://github.com/zyantific/zydis.git
        GIT_TAG v4.1.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Zydis)
    target_compile_definitions(ReZeroDetour PRIVATE ZYDIS_STATIC_DEFINE)
    target_compile_definitions(ReZeroDetour PRIVATE
        ZYDIS_DISABLE_ENCODER
        ZYDIS_DISABLE_FORMATTER
    )
    target_link_libraries(ReZeroDetour PUBLIC Zydis)
elseif(REZERODETOUR_DISASSEMBLER STREQUAL "BDDISASM")
    message(FATAL_ERROR "BDDISASM backend is not implemented yet.")
elseif(REZERODETOUR_DISASSEMBLER STREQUAL "HDE64")
    message(FATAL_ERROR "HDE64 backend is not implemented yet.")
endif()

FetchContent_Declare(
    SmpAllocator
    GIT_REPOSITORY https://github.com/ReZeroShay/SmpAllocator.git
    GIT_TAG main
)
FetchContent_MakeAvailable(SmpAllocator)

target_link_libraries(ReZeroDetour PUBLIC SmpAllocator::SmpAllocator)

if(REZERODETOUR_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
