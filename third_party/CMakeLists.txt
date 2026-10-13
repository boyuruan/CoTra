# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT license.

# Parameters:
#
# BOOST_ROOT:
#   Specify root of the Boost library if Boost cannot be auto-detected. On Windows, a fallback to a
#   downloaded nuget version will be used if Boost cannot be found.
#
# DISKANN_RELEASE_UNUSED_TCMALLOC_MEMORY_AT_CHECKPOINTS:
#   This is a work-in-progress feature, not completed yet. The core DiskANN library will be split into
#   build-related and search-related functionality. In build-related functionality, when using tcmalloc,
#   it's possible to release memory that's free but reserved by tcmalloc. Setting this to true enables
#   such behavior.
#   Contact for this feature: gopalrs.


# Some variables like MSVC are defined only after project(), so put that first.
# cmake_minimum_required(VERSION 3.15)
# project(diskann)

set(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/diskann")
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")


include_directories(${PROJECT_SOURCE_DIR}/include)

if(NOT PYBIND)
set(DISKANN_RELEASE_UNUSED_TCMALLOC_MEMORY_AT_CHECKPOINTS ON)
endif()
# It's necessary to include tcmalloc headers only if calling into MallocExtension interface.
# For using tcmalloc in DiskANN tools, it's enough to just link with tcmalloc.
if (DISKANN_RELEASE_UNUSED_TCMALLOC_MEMORY_AT_CHECKPOINTS)
include_directories(${PROJECT_SOURCE_DIR}/gperftools/src)
endif()

include_directories(/usr/local/include)

# The parent finds OpenBLAS and libaio. compat/mkl.h supplies the MKL names
# this tree includes. LP64: do not define MKL_ILP64.
if(NOT TARGET OpenBLAS::OpenBLAS)
    message(FATAL_ERROR "OpenBLAS was not found. Configure from the top-level project.")
endif()
if(NOT AIO_LIB)
    message(FATAL_ERROR "libaio was not found.")
endif()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/compat)

# DiskANN core uses header-only libraries. Only DiskANN tools need program_options which has a linker library,
# but its size is small. Reduce number of dependent DLLs by linking statically.
if(NOT MSVC)
find_package(Boost COMPONENTS program_options)
endif()

if (NOT Boost_FOUND)
    message(FATAL_ERROR "Couldn't find Boost dependency")
endif()

include_directories(${Boost_INCLUDE_DIR})

add_compile_options(-m64)

# Section for tcmalloc. The DiskANN tools are always linked to tcmalloc. For Windows, they also need to
# force-include the _tcmalloc symbol for enabling tcmalloc.
#
# The DLL itself needs to be linked to tcmalloc only if DISKANN_RELEASE_UNUSED_TCMALLOC_MEMORY_AT_CHECKPOINTS
# is enabled.
set(DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS "-ltcmalloc")

if (DISKANN_RELEASE_UNUSED_TCMALLOC_MEMORY_AT_CHECKPOINTS)
    add_definitions(-DRELEASE_UNUSED_TCMALLOC_MEMORY_AT_CHECKPOINTS)
endif()

if (NOT MSVC)
    set(DISKANN_ASYNC_LIB ${AIO_LIB})
endif()


set(ENV{TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD} 500000000000)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mfma -msse2 -ftree-vectorize -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fopenmp -fopenmp-simd -funroll-loops -Wfatal-errors -DUSE_AVX2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG -fPIC")


file(GLOB CPP_SOURCES diskann/src/*.cpp)
if (RESTAPI)
    list(APPEND CPP_SOURCES diskann/src/restapi/search_wrapper.cpp diskann/src/restapi/server.cpp)
endif()
add_library(diskann ${CPP_SOURCES})
# Ubuntu's libopenblas exports CBLAS but not LAPACKE. liblapacke is the C
# wrapper; its libblas/liblapack dependencies are OpenBLAS via alternatives.
find_library(LAPACKE_LIB lapacke REQUIRED)
target_link_libraries(diskann PUBLIC OpenBLAS::OpenBLAS ${LAPACKE_LIB} ${AIO_LIB})
target_include_directories(diskann PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/compat)

if (RESTAPI)
    add_subdirectory(diskann/apps/restapi)
endif()

include(diskann/clang-format.cmake)

if(PYBIND)
    add_subdirectory(diskann/python)
endif()


