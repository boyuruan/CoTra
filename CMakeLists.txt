cmake_minimum_required(VERSION 3.16)
project(rdma-anns)

# Compiler and library locations come from the environment. Index builds use
# LP64 OpenBLAS through third_party/compat/mkl.h, not the cluster ILP64 MKL.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fcoroutines COTRA_HAS_FCOROUTINES)

add_compile_options(
  -Ofast -march=native -fpic -w -fopenmp -ftree-vectorize)
if(COTRA_HAS_FCOROUTINES)
  add_compile_options(-fcoroutines)
endif()
add_link_options(-fopenmp)

# MACHINE_NUM is the partition count. A command-line -DMACHINE_NUM=N overrides
# the header default. An index built with one value cannot be read by another.
set(MACHINE_NUM "16" CACHE STRING "Compile-time machine and partition count")
add_compile_definitions(MACHINE_NUM=${MACHINE_NUM})

option(COTRA_WITH_INDEX
  "Build the index and ANN binaries. Needs OpenBLAS, LAPACK, and libaio."
  OFF)

find_package(Threads REQUIRED)
find_package(Boost 1.58.0 REQUIRED
  COMPONENTS serialization iostreams program_options system filesystem)
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
pkg_check_modules(RDMACM REQUIRED IMPORTED_TARGET librdmacm)
find_library(MEMCACHED_LIB memcached REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/third_party)

if(COTRA_WITH_INDEX)
  # Fedora ships OpenBLAS without openblas.pc. Debian and the Intel oneAPI
  # layout do provide it.
  pkg_check_modules(OPENBLAS IMPORTED_TARGET openblas)
  if(TARGET PkgConfig::OPENBLAS)
    add_library(OpenBLAS::OpenBLAS ALIAS PkgConfig::OPENBLAS)
  else()
    find_path(OPENBLAS_INCLUDE_DIR NAMES cblas.h PATH_SUFFIXES openblas
      REQUIRED)
    find_library(OPENBLAS_LIB NAMES openblas REQUIRED)
    add_library(OpenBLAS::OpenBLAS UNKNOWN IMPORTED)
    set_target_properties(OpenBLAS::OpenBLAS PROPERTIES
      IMPORTED_LOCATION "${OPENBLAS_LIB}"
      INTERFACE_INCLUDE_DIRECTORIES "${OPENBLAS_INCLUDE_DIR}")
  endif()
  find_library(AIO_LIB aio REQUIRED)
  add_subdirectory(${CMAKE_SOURCE_DIR}/third_party)
endif()

add_custom_target(lib)
add_subdirectory(coromem)

add_library(rdma_anns)
file(GLOB sources
  ${CMAKE_SOURCE_DIR}/src/*.cc
  ${CMAKE_SOURCE_DIR}/src/rdma/*.cc
  ${CMAKE_SOURCE_DIR}/src/anns/*.cc
)
target_sources(rdma_anns PRIVATE ${sources})
target_include_directories(rdma_anns PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  # Bare includes ("logger.h") inside DiskANN headers. Not global: coromem also
  # has a utils.h, and a global -I would hide it.
  ${CMAKE_SOURCE_DIR}/third_party/diskann/include
)
target_link_libraries(rdma_anns PUBLIC
  coromem
  Threads::Threads
  PkgConfig::IBVERBS
  PkgConfig::RDMACM
  ${MEMCACHED_LIB}
  Boost::program_options
)

if(COTRA_WITH_INDEX)
  add_library(rdma_index)
  file(GLOB index_sources ${CMAKE_SOURCE_DIR}/src/index/*.cc)
  target_sources(rdma_index PRIVATE ${index_sources})
  target_include_directories(rdma_index PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${CMAKE_SOURCE_DIR}/third_party/compat
  )
  target_link_libraries(rdma_index PUBLIC OpenBLAS::OpenBLAS)
endif()

add_subdirectory(tests)
