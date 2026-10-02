cmake_minimum_required(VERSION 3.20)
project(z80-lift LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Projects on top of core, like LLVM_ENABLE_PROJECTS. The tester needs the
# lifter.
set(Z80LIFT_ENABLE_PROJECTS "lifter;tester" CACHE STRING
    "Projects to build on top of core: lifter, tester")
set(projects ${Z80LIFT_ENABLE_PROJECTS})
foreach(proj IN LISTS projects)
  if(NOT proj MATCHES "^(lifter|tester)$")
    message(FATAL_ERROR "Unknown project ${proj} in Z80LIFT_ENABLE_PROJECTS")
  endif()
endforeach()
if("tester" IN_LIST projects AND NOT "lifter" IN_LIST projects)
  list(APPEND projects lifter)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(Bitcode)

# Build with the same definitions and RTTI setting as LLVM.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

# z80lift_llvm_libs(<var> <component>...): the libraries for the components,
# or the single LLVM library when LLVM is built as one.
function(z80lift_llvm_libs var)
  if(LLVM_LINK_LLVM_DYLIB)
    set(${var} LLVM PARENT_SCOPE)
  else()
    llvm_map_components_to_libnames(libs ${ARGN})
    set(${var} ${libs} PARENT_SCOPE)
  endif()
endfunction()

add_subdirectory(core)
if("lifter" IN_LIST projects)
  add_subdirectory(lifter)
endif()
if("tester" IN_LIST projects)
  add_subdirectory(tester)
endif()
