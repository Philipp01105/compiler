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

set(fallback "${OUTPUT_DIR}/function_fallback.s")
execute_process(
        COMMAND "${COMPILER}" --deterministic -o "${fallback}"
                "${ROOT}/tests/execution/functions/function.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Compatibility emission failed: ${errors}")
endif()
file(READ "${fallback}" assembly)
if(NOT assembly MATCHES "# Lowering: compatibility")
    message(FATAL_ERROR "unsupported IR module did not use compatibility lowering")
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
    if(target STREQUAL "coff")
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
    if(NOT result STREQUAL "0" OR NOT output_text STREQUAL "3" OR NOT errors STREQUAL "")
        message(FATAL_ERROR "IR-native integer call runtime failed (${result}): ${output_text}${errors}")
    endif()
    endif()
  endforeach()
endforeach()
