# Builds LLVM and clang from the llvm-z80 submodule in <build>/llvm, with the
# X86 and Z80 targets and with RTTI and exceptions, which Alive2 needs.

set(Z80LIFT_LLVM_SOURCE_DIR ${PROJECT_SOURCE_DIR}/llvm-z80/llvm)
set(Z80LIFT_LLVM_BINARY_DIR ${CMAKE_BINARY_DIR}/llvm)

# Configures LLVM if it is not yet and points LLVM_DIR at it. The build tree
# has its CMake package once it is configured, so LLVM itself is built later
# by the llvm target.
function(z80lift_configure_llvm)
  if(NOT EXISTS ${Z80LIFT_LLVM_SOURCE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "llvm-z80 is missing; run 'git submodule update "
      "--init', or pass -DLLVM_DIR=<llvm>/lib/cmake/llvm")
  endif()
  if(NOT EXISTS ${Z80LIFT_LLVM_BINARY_DIR}/CMakeCache.txt)
    set(args
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DLLVM_ENABLE_PROJECTS=clang
      -DLLVM_TARGETS_TO_BUILD=X86
      -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Z80
      -DLLVM_ENABLE_ASSERTIONS=ON
      -DLLVM_ENABLE_RTTI=ON
      -DLLVM_ENABLE_EH=ON)
    find_program(Z80LIFT_CCACHE ccache)
    if(Z80LIFT_CCACHE)
      list(APPEND args -DLLVM_CCACHE_BUILD=ON)
    endif()
    find_program(Z80LIFT_LLD ld.lld)
    if(Z80LIFT_LLD)
      list(APPEND args -DLLVM_USE_LINKER=lld)
    endif()
    message(STATUS "Configuring LLVM in ${Z80LIFT_LLVM_BINARY_DIR}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -G ${CMAKE_GENERATOR}
              -S ${Z80LIFT_LLVM_SOURCE_DIR} -B ${Z80LIFT_LLVM_BINARY_DIR}
              ${args}
      RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Configuring LLVM failed")
    endif()
  endif()
  set(LLVM_DIR ${Z80LIFT_LLVM_BINARY_DIR}/lib/cmake/llvm CACHE PATH
      "The LLVM to build against" FORCE)
endfunction()

# Adds the llvm target, which builds the LLVM libraries, clang and llvm-link.
# The files are listed as its products so that Ninja knows where they come
# from.
function(z80lift_add_llvm_target)
  set(files ${LLVM_TOOLS_BINARY_DIR}/clang ${LLVM_TOOLS_BINARY_DIR}/llvm-link)
  foreach(lib IN LISTS LLVM_AVAILABLE_LIBS)
    if(TARGET ${lib})
      get_target_property(file ${lib} IMPORTED_LOCATION_RELEASE)
      if(file)
        list(APPEND files ${file})
      endif()
    endif()
  endforeach()
  add_custom_target(llvm
    COMMAND ${CMAKE_COMMAND} --build ${Z80LIFT_LLVM_BINARY_DIR}
            --target llvm-libraries clang llvm-link
    BYPRODUCTS ${files}
    USES_TERMINAL
    COMMENT "Building LLVM and clang")
endfunction()

# Makes every target in <dir> and its subdirectories wait for the llvm target,
# since they include headers that the LLVM build generates.
function(z80lift_depend_on_llvm dir)
  get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    if(NOT target STREQUAL "llvm")
      add_dependencies(${target} llvm)
    endif()
  endforeach()
  get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    z80lift_depend_on_llvm(${subdir})
  endforeach()
endfunction()
