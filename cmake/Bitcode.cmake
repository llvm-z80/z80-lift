# Builds bitcode with a clang matching the LLVM we link against and embeds it in
# a tool, so that nothing has to be found at run time.

find_program(Z80LIFT_CLANG NAMES clang HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(Z80LIFT_LLVM_LINK NAMES llvm-link HINTS ${LLVM_TOOLS_BINARY_DIR}
             REQUIRED)

execute_process(COMMAND ${Z80LIFT_CLANG} -dumpversion
                OUTPUT_VARIABLE _z80lift_clang_version
                OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "^[0-9]+" _z80lift_clang_major "${_z80lift_clang_version}")
if(NOT _z80lift_clang_major STREQUAL LLVM_VERSION_MAJOR)
  message(WARNING "${Z80LIFT_CLANG} is ${_z80lift_clang_version} but LLVM is "
                  "${LLVM_PACKAGE_VERSION}; the embedded bitcode may not load.")
endif()

# z80lift_add_bitcode(<target> NAMESPACE <ns> FUNCTION <name>
#                     SOURCES <src>... [INCLUDES <dir>...] [FLAGS <flag>...])
#
# Compiles SOURCES to one bitcode module and adds a static library <target>
# that returns it from `llvm::StringRef <ns>::<name>()`.
function(z80lift_add_bitcode target)
  cmake_parse_arguments(ARG "" "NAMESPACE;FUNCTION" "SOURCES;INCLUDES;FLAGS"
                        ${ARGN})
  set(dir ${CMAKE_CURRENT_BINARY_DIR}/${target}.dir)
  file(MAKE_DIRECTORY ${dir})

  set(includes)
  foreach(inc IN LISTS ARG_INCLUDES)
    list(APPEND includes -I${inc})
  endforeach()

  set(bcs)
  foreach(src IN LISTS ARG_SOURCES)
    get_filename_component(abs ${src} ABSOLUTE)
    string(MAKE_C_IDENTIFIER ${src} name)
    set(bc ${dir}/${name}.bc)
    add_custom_command(
      OUTPUT ${bc}
      COMMAND ${Z80LIFT_CLANG} -c -emit-llvm -O2 ${ARG_FLAGS} ${includes}
              -MD -MF ${bc}.d ${abs} -o ${bc}
      DEPENDS ${abs}
      DEPFILE ${bc}.d
      COMMENT "Building bitcode ${src}"
      VERBATIM)
    list(APPEND bcs ${bc})
  endforeach()

  set(module ${dir}/${ARG_FUNCTION}.bc)
  add_custom_command(
    OUTPUT ${module}
    COMMAND ${Z80LIFT_LLVM_LINK} ${bcs} -o ${module}
    DEPENDS ${bcs}
    COMMENT "Linking bitcode for ${ARG_NAMESPACE}::${ARG_FUNCTION}"
    VERBATIM)

  set(cpp ${dir}/${ARG_FUNCTION}.cpp)
  add_custom_command(
    OUTPUT ${cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${module} -DOUTPUT=${cpp}
            -DNAMESPACE=${ARG_NAMESPACE} -DFUNCTION=${ARG_FUNCTION}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBitcode.cmake
    DEPENDS ${module} ${PROJECT_SOURCE_DIR}/cmake/EmbedBitcode.cmake
    COMMENT "Embedding bitcode for ${ARG_NAMESPACE}::${ARG_FUNCTION}"
    VERBATIM)

  add_library(${target} STATIC ${cpp})
endfunction()
