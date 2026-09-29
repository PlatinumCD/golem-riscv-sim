# Mordred Top-Level CMake
# Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
# All Rights Reserved
# contact@tactcomplabs.com
# See LICENSE in the top level directory for licensing details

# ---------------------------------------------------
# string_contains_number
# ---------------------------------------------------
function(string_contains_number str result)
  string(REGEX MATCH "[0-9]" has_number "${str}")
  if(has_number)
    set(${result} TRUE PARENT_SCOPE)
  else()
    set(${result} FALSE PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------
# main
# ---------------------------------------------------
# Prevent in-source builds
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR "DO NOT BUILD in-tree.")
endif()

# Minimum required version of CMake and project information
cmake_minimum_required(VERSION 3.19)
project(mordred CXX)

# SST Configuration Sanity Check
find_program(SST sst)
find_program(SST_CONFIG sst-config)
if(NOT SST OR NOT SST_CONFIG)
    message(FATAL_ERROR "No SST binary or sst-config binary found in path")
endif()

# SST Environment Derivation
execute_process(COMMAND sst-config --prefix
        OUTPUT_VARIABLE SST_INSTALL_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(SST_INCLUDE_DIR "${SST_INSTALL_DIR}/include")
if(NOT (EXISTS "${SST_INSTALL_DIR}"))
    message(FATAL_ERROR " SST_INSTALL_DIR (${SST_INSTALL_DIR}) is invalid.")
endif()
include_directories(SST_SRC ${SST_INSTALL_DIR})

execute_process(COMMAND sst-config --CXX
        OUTPUT_VARIABLE CXX
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(COMMAND sst-config --ELEMENT_CXXFLAGS
        OUTPUT_VARIABLE SST_CXXFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CXXFLAGS "${SST_CXXFLAGS}  -fno-stack-protector")
execute_process(COMMAND sst-config --ELEMENT_LDFLAGS
        OUTPUT_VARIABLE SST_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(LDFLAGS "${SST_LDFLAGS}  -fno-stack-protector")


execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sst-major-version.sh
                OUTPUT_VARIABLE SST_MAJOR_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sst-minor-version.sh
                OUTPUT_VARIABLE SST_MINOR_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
)

string_contains_number("${SST_MAJOR_VERSION}" contains_num)
if(NOT contains_num)
  set(SST_MAJOR_VERSION "DEV")
  set(SST_MINOR_VERSION "DEV")
  execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sst-git-hash.sh
                  OUTPUT_VARIABLE SST_GIT_HASH
                  OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  message(STATUS "SST GIT HASH=${SST_GIT_HASH}")
endif()

message(STATUS "SST MAJOR VERSION=${SST_MAJOR_VERSION}")
message(STATUS "SST MINOR VERSION=${SST_MINOR_VERSION}")
string(CONCAT SST_VERSION  ${SST_MAJOR_VERSION} "." ${SST_MINOR_VERSION})
message(STATUS "SST FULL VERSION=${SST_VERSION}")

if(NOT "${SST_MAJOR_VERSION}" STREQUAL "DEV")
  if(${SST_VERSION} VERSION_LESS "15.0")
    message(FATAL_ERROR "Mordred requires SST 15.0.0+")
  endif()
  add_compile_definitions(SST_MAJOR_VERSION=${SST_MAJOR_VERSION})
else()
  # DEV builds track main and are assumed to be SST 16+
  add_compile_definitions(SST_MAJOR_VERSION=16)
endif()

# Compiler Options
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(FP_MODE_FLAG "-ffp-model=strict -frounding-math -ftrapping-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-command-line-argument")
else()
    set(FP_MODE_FLAG "-frounding-math -ftrapping-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wmissing-format-attribute -Wsuggest-final-methods -Wsuggest-final-types -Wvolatile")
endif()

set(CMAKE_CXX_FLAGS "-std=c++17 ${FP_MODE_FLAG} -O2 -Wall -Wextra -Wsuggest-override -Wmissing-noreturn -Wvla -Wuninitialized -Wdouble-promotion -Wsign-conversion -Wno-unused-parameter -Wno-deprecated-declarations -Wno-macro-redefined -Werror ${CMAKE_CXX_FLAGS} -I./ ${LDFLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -Wall")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall")

# Build options
option(MORDRED_ENABLE_PHYS_CHANNEL
    "Build PhysChannel-backed components (RtrPortControlPC, MordredNicPC). Requires prydwen."
    ON)

# Turn on testing
enable_testing()

# Subdirectories
add_subdirectory(src)
add_subdirectory(tests)

# EOF
