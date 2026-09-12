cmake_minimum_required(VERSION 3.21)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED COMPILER OR NOT EXISTS "${COMPILER}")
    message(FATAL_ERROR "Set COMPILER to a freshly built compiler executable")
endif()
if(NOT DEFINED OUTPUT_DIR)
    set(OUTPUT_DIR "${ROOT}/test_output/ir_backend")
endif()
if(NOT DEFINED ASSEMBLER OR NOT EXISTS "${ASSEMBLER}")
    message(FATAL_ERROR "Set ASSEMBLER to a C compiler capable of linking generated assembly")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# Emit every supported object format, but only assemble and execute the format
# understood by the host toolchain. Linux GCC cannot link COFF assembly and
# MinGW cannot link ELF assembly.
if(WIN32)
    set(HOST_TARGET coff)
else()
    set(HOST_TARGET elf)
endif()

foreach(target elf coff)
    foreach(syntax att intel)
        set(output "${OUTPUT_DIR}/hello_${target}_${syntax}.s")
        execute_process(
                COMMAND "${COMPILER}" --deterministic "--target=${target}"
                        "--syntax=${syntax}" -o "${output}"
                        "${ROOT}/tests/execution/basics/hello.dmm"
                RESULT_VARIABLE result ERROR_VARIABLE errors)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "IR-native emission failed: ${errors}")
        endif()
        file(READ "${output}" assembly)
        if(NOT assembly MATCHES "# Lowering: typed IR")
            message(FATAL_ERROR "hello did not use typed IR (${target}/${syntax})")
        endif()
    endforeach()
endforeach()

foreach(case conditional break_continue)
    set(source "${ROOT}/tests/execution/control_flow/${case}.dmm")
    set(output "${OUTPUT_DIR}/${case}_native.s")
    execute_process(
            COMMAND "${COMPILER}" --deterministic --target=elf --syntax=intel
                    -o "${output}" "${source}"
            RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "IR-native ${case} emission failed: ${errors}")
    endif()
    file(READ "${output}" assembly)
    if(NOT assembly MATCHES "# Lowering: typed IR")
        message(FATAL_ERROR "${case} did not use typed IR")
    endif()
endforeach()

set(function_output "${OUTPUT_DIR}/function_native.s")
execute_process(
        COMMAND "${COMPILER}" --deterministic -o "${function_output}"
                "${ROOT}/tests/execution/functions/function.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "IR-native function emission failed: ${errors}")
endif()
file(READ "${function_output}" assembly)
if(NOT assembly MATCHES "# Lowering: typed IR")
    message(FATAL_ERROR "integer function/string stream did not use typed IR")
endif()

foreach(target elf coff)
  foreach(syntax att intel)
    set(output "${OUTPUT_DIR}/integer_call_${target}_${syntax}.s")
    set(executable "${OUTPUT_DIR}/integer_call_${target}_${syntax}.exe")
    execute_process(
            COMMAND "${COMPILER}" --deterministic "--target=${target}" "--syntax=${syntax}"
                    -o "${output}" "${ROOT}/tests/unit/frontend_ast_fixture.dmm"
            RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "IR-native integer call emission failed: ${errors}")
    endif()
    file(READ "${output}" assembly)
    if(NOT assembly MATCHES "# Lowering: typed IR" OR NOT assembly MATCHES "call add")
        message(FATAL_ERROR "integer function call did not use typed IR (${target}/${syntax})")
    endif()
    if(target STREQUAL "${HOST_TARGET}")
    execute_process(
            COMMAND "${ASSEMBLER}" -no-pie "${output}" -o "${executable}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output_text ERROR_VARIABLE errors)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "IR-native integer call assembly failed: ${errors}")
    endif()
    execute_process(
            COMMAND "${executable}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output_text ERROR_VARIABLE errors)
    string(REPLACE "\r\n" "\n" output_text "${output_text}")
    string(REGEX REPLACE "\n+$" "" output_text "${output_text}")
    if(NOT result STREQUAL "0" OR NOT output_text STREQUAL "" OR NOT errors STREQUAL "")
        message(FATAL_ERROR "IR-native integer call runtime failed (${result}): ${output_text}${errors}")
    endif()
    endif()
  endforeach()
endforeach()

foreach(case operator_precedence short_circuit)
    set(source "${ROOT}/tests/execution/expressions/${case}.dmm")
    set(output "${OUTPUT_DIR}/${case}_native.s")
    execute_process(
            COMMAND "${COMPILER}" --deterministic --target=elf --syntax=intel
                    -o "${output}" "${source}"
            RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "IR-native ${case} emission failed: ${errors}")
    endif()
    file(READ "${output}" assembly)
    if(NOT assembly MATCHES "# Lowering: typed IR")
        message(FATAL_ERROR "${case} did not use typed IR")
    endif()
endforeach()

set(float_output "${OUTPUT_DIR}/float_literal_native.s")
execute_process(
        COMMAND "${COMPILER}" --deterministic --target=elf --syntax=intel
                -o "${float_output}" "${ROOT}/tests/execution/expressions/float_literal.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "IR-native float literal emission failed: ${errors}")
endif()
file(READ "${float_output}" assembly)
if(NOT assembly MATCHES "# Lowering: typed IR")
    message(FATAL_ERROR "float literal did not use typed IR")
endif()

foreach(target elf coff)
  foreach(syntax att intel)
    set(output "${OUTPUT_DIR}/scalar_${target}_${syntax}.s")
    set(executable "${OUTPUT_DIR}/scalar_${target}_${syntax}.exe")
    execute_process(
            COMMAND "${COMPILER}" --deterministic "--target=${target}" "--syntax=${syntax}"
                    -o "${output}" "${ROOT}/tests/unit/ir_scalar_fixture.dmm"
            RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "IR-native scalar emission failed: ${errors}")
    endif()
    file(READ "${output}" assembly)
    if(NOT assembly MATCHES "# Lowering: typed IR")
        message(FATAL_ERROR "scalar program did not use typed IR (${target}/${syntax})")
    endif()
    if(target STREQUAL "${HOST_TARGET}")
      execute_process(
              COMMAND "${ASSEMBLER}" -no-pie "${output}" -o "${executable}"
              RESULT_VARIABLE result ERROR_VARIABLE errors)
      if(NOT result STREQUAL "0")
          message(FATAL_ERROR "IR-native scalar assembly failed: ${errors}")
      endif()
      execute_process(
              COMMAND "${executable}"
              RESULT_VARIABLE result OUTPUT_VARIABLE output_text ERROR_VARIABLE errors)
      string(REPLACE "\r\n" "\n" output_text "${output_text}")
      string(REGEX REPLACE "\n+$" "" output_text "${output_text}")
      if(NOT result STREQUAL "0" OR NOT output_text STREQUAL "20\n-20\n1" OR
         NOT errors STREQUAL "")
          message(FATAL_ERROR
                  "IR-native scalar runtime failed (${result}): ${output_text}${errors}")
      endif()
    endif()
  endforeach()
endforeach()
