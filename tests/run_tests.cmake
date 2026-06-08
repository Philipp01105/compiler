include("${CMAKE_CURRENT_LIST_DIR}/source_fixture.cmake")
cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/standalone_link.cmake")
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED COMPILER OR NOT EXISTS "${COMPILER}")
    message(FATAL_ERROR "Set COMPILER to a freshly built compiler executable")
endif()

if(NOT DEFINED ASSEMBLER)
    find_program(ASSEMBLER NAMES gcc REQUIRED)
endif()

if(NOT DEFINED OUTPUT_DIR)
    set(OUTPUT_DIR "${ROOT}/test_output/regression")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
dmm_test_write( "${OUTPUT_DIR}/dmm.manifest" "module dmm.test/regression\ndmm 0.3\n")
get_filename_component(OUTPUT_DIR "${OUTPUT_DIR}" ABSOLUTE)
get_filename_component(COMPILER "${COMPILER}" ABSOLUTE)

if(NOT DEFINED TEST_STAGE)
    set(TEST_STAGE all)
endif()

if(NOT DEFINED SANITIZER_FLAGS)
    set(SANITIZER_FLAGS "")
endif()

function(run_case source expected syntax)
    get_filename_component(name "${source}" NAME_WE)
    set(work "${OUTPUT_DIR}/${name}_${syntax}")
    file(MAKE_DIRECTORY "${work}")
    file(COPY_FILE "${source}" "${work}/input.dmm")
    get_filename_component(source_directory "${source}" DIRECTORY)
    if(EXISTS "${source_directory}/dmm.manifest")
        file(COPY_FILE "${source_directory}/dmm.manifest" "${work}/dmm.manifest")
    endif()

    execute_process(
            COMMAND "${COMPILER}" --emit=asm "--syntax=${syntax}" "${work}/input.dmm"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors
            TIMEOUT 30
    )

    if(NOT result STREQUAL "0")
        message(FATAL_ERROR
                "${name}/${syntax}: compilation failed (${result})\n${output}${errors}"
        )
    endif()

    execute_process(
            COMMAND "${ASSEMBLER}" ${STANDALONE_FLAGS}
            "${work}/input.dmm.s"
            ${SYSTEM_LIBRARIES} -o "${work}/program.exe"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors
            TIMEOUT 30
    )

    if(NOT result STREQUAL "0")
        message(FATAL_ERROR
                "${name}/${syntax}: assembly failed (${result})\n${output}${errors}"
        )
    endif()
    if(name STREQUAL "hello")
        check_standalone_dependencies("${work}/program.exe")
    endif()

    execute_process(
            COMMAND "${work}/program.exe"
            WORKING_DIRECTORY "${work}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors
            TIMEOUT 10
    )

    # Store the original output for debugging.
    dmm_test_write( "${work}/actual.out" "${output}")

    # Normalize platform-specific line endings.
    #
    # Windows commonly uses CRLF (\r\n), while Linux uses LF (\n).
    # Normalize both output and expected output to LF so that tests
    # behave identically on Windows and Linux.
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r" "\n" output "${output}")

    string(REPLACE "\r\n" "\n" expected "${expected}")
    string(REPLACE "\r" "\n" expected "${expected}")

    # Ignore differences caused only by trailing newline characters.
    #
    # Other whitespace remains significant. For example:
    #
    #   "Hello World"
    #
    # is still different from:
    #
    #   "HelloWorld"
    #
    # Only final newline differences at EOF are ignored.
    string(REGEX REPLACE "\n+$" "" output "${output}")
    string(REGEX REPLACE "\n+$" "" expected "${expected}")

    if(
            NOT result STREQUAL "0"
            OR NOT output STREQUAL expected
            OR NOT errors STREQUAL ""
    )
        message(FATAL_ERROR
                "${name}/${syntax}: runtime/output failure (${result}); "
                "see ${work}/actual.out\n${errors}"
        )
    endif()

    message(STATUS "PASS ${name}/${syntax}")
endfunction()


# ---------------------------------------------------------------------------
# Positive execution tests
# ---------------------------------------------------------------------------

if(TEST_STAGE STREQUAL "all" OR TEST_STAGE STREQUAL "positive")

    if(NOT DEFINED TEST_FILES)
        file(
                GLOB_RECURSE TEST_FILES
                "${ROOT}/tests/execution/*.dmm"
                "${ROOT}/tests/regression/*.dmm"
        )
    endif()

    foreach(source IN LISTS TEST_FILES)
        get_filename_component(name "${source}" NAME_WE)
        get_filename_component(source_dir "${source}" DIRECTORY)

        set(expected_file "${source_dir}/${name}.expected")

        if(NOT EXISTS "${expected_file}")
            set(expected_file "${ROOT}/tests/expected/${name}.expected")
        endif()

        if(NOT EXISTS "${expected_file}")
            message(FATAL_ERROR
                    "Missing expected output: ${expected_file}"
            )
        endif()

        file(READ "${expected_file}" expected)

        foreach(syntax intel att)
            run_case("${source}" "${expected}" "${syntax}")
        endforeach()
    endforeach()

endif()


# ---------------------------------------------------------------------------
# ABI tests
# Assemble with a renamed DMM entry point and call its functions from C.
# ---------------------------------------------------------------------------

if(TEST_STAGE STREQUAL "all" OR TEST_STAGE STREQUAL "abi")

    foreach(syntax intel att)
        set(work "${OUTPUT_DIR}/abi_${syntax}")
        file(MAKE_DIRECTORY "${work}")

        file(
                COPY_FILE
                "${ROOT}/tests/abi/systemv/core_abi/core_abi.dmm"
                "${work}/input.dmm"
        )

        execute_process(
                COMMAND "${COMPILER}" --emit=asm "--syntax=${syntax}" "${work}/input.dmm"
                RESULT_VARIABLE result
                ERROR_VARIABLE errors
                TIMEOUT 30
        )

        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "ABI compile failed: ${errors}")
        endif()

        file(READ "${work}/input.dmm.s" assembly)
        string(REPLACE "main" "dmm_test_entry" assembly "${assembly}")
        dmm_test_write( "${work}/interop.s" "${assembly}")
        dmm_test_abi_definitions("${work}/interop.s" abi_definitions)

        execute_process(
                COMMAND "${ASSEMBLER}"
                ${SANITIZER_FLAGS}
                -no-pie
                ${abi_definitions}
                "${work}/interop.s"
                "${ROOT}/tests/abi/c_interop/abi_driver.c"
                ${SYSTEM_LIBRARIES} -o "${work}/interop.exe"
                RESULT_VARIABLE result
                ERROR_VARIABLE errors
                TIMEOUT 30
        )

        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "ABI link failed: ${errors}")
        endif()

        execute_process(
                COMMAND "${work}/interop.exe"
                RESULT_VARIABLE result
                OUTPUT_VARIABLE output
                ERROR_VARIABLE errors
                TIMEOUT 10
        )

        if(
                NOT result STREQUAL "0"
                OR NOT output MATCHES "GCC ABI interop OK"
        )
            message(FATAL_ERROR
                    "ABI runtime failed (${result}): ${output}${errors}"
            )
        endif()

        message(STATUS "PASS gcc-interop/${syntax}")
    endforeach()

endif()


# ---------------------------------------------------------------------------
# Rejection tests
# ---------------------------------------------------------------------------

function(reject_case name source diagnostic)

    set(work "${OUTPUT_DIR}/reject_${name}")
    file(MAKE_DIRECTORY "${work}")

    dmm_test_write( "${work}/input.dmm" "${source}")
    dmm_test_write( "${work}/input.dmm.s" "stale output")

    execute_process(
            COMMAND "${COMPILER}" --emit=asm "${work}/input.dmm"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors
            TIMEOUT 30
    )

    dmm_test_write(
            "${work}/diagnostics.log"
            "${output}${errors}"
    )

    string(FIND "${errors}" "${diagnostic}" diagnostic_position)

    if(
            NOT result STREQUAL "1"
            OR diagnostic_position EQUAL -1
    )
        message(FATAL_ERROR
                "${name}: expected rejection matching '${diagnostic}', "
                "got ${result}; see ${work}/diagnostics.log"
        )
    endif()

    if(EXISTS "${work}/input.dmm.s")
        # A failed frontend can leave no source inventory to validate deletion.
        # In that case retain the old file, but never write new assembly.
        file(READ "${work}/input.dmm.s" remaining_output)
        if(NOT remaining_output STREQUAL "stale output" OR NOT errors MATCHES "error\\[L[0-9]+\\]")
            message(FATAL_ERROR "${name}: rejected source produced assembly")
        endif()
    endif()

    message(STATUS "PASS rejection/${name}")

endfunction()


if(TEST_STAGE STREQUAL "all" OR TEST_STAGE STREQUAL "rejection")

    file(
            GLOB_RECURSE REJECTION_FILES
            "${ROOT}/tests/rejection/*.dmm"
    )

    foreach(source IN LISTS REJECTION_FILES)

        get_filename_component(name "${source}" NAME_WE)
        get_filename_component(source_dir "${source}" DIRECTORY)

        set(
                diagnostic_file
                "${source_dir}/${name}.diag"
        )

        if(NOT EXISTS "${diagnostic_file}")
            message(FATAL_ERROR
                    "Missing diagnostic expectation: ${diagnostic_file}"
            )
        endif()

        file(READ "${source}" rejection_source)
        file(READ "${diagnostic_file}" diagnostic)

        string(STRIP "${diagnostic}" diagnostic)

        reject_case(
                "${name}"
                "${rejection_source}"
                "${diagnostic}"
        )

    endforeach()

    reject_case(
            large_array
            "import <stdlib>\nfunc main() -> void { var a:int[2147483647]; }"
            "Array length"
    )

    reject_case(
            combined_locals
            "import <stdlib>\nfunc main() -> void { var a:double[1048576]; var b:double[1048576]; var c:int; }"
            "local storage exceeds"
    )

    reject_case(
            wrong_argument_type
            "func f(a:int) -> int { return a; } func main() -> void { f(\"bad\"); }"
            "No overload of 'f' matches"
    )

    reject_case(
            wrong_argument_count
            "func f(a:int) -> int { return a; } func main() -> void { f(); }"
            "No overload of 'f' matches"
    )

    reject_case(
            wrong_initializer
            "import <stdlib>\nfunc main() -> void { var n:int = \"bad\"; }"
            "Cannot implicitly convert"
    )

    reject_case(
            narrowing
            "import <stdlib>\nfunc main() -> void { var n:int = 1.5; }"
            "Cannot implicitly convert"
    )

    reject_case(
            wrong_return
            "func f() -> int { return \"bad\"; } func main() -> void {}"
            "Cannot implicitly convert"
    )

    reject_case(
            missing_return
            "func f(n:int) -> int { if (n>0) { return n; } } func main() -> void {}"
            "does not return on all paths"
    )

    reject_case(
            void_return
            "import <stdlib>\nfunc main() -> void { return 1; }"
            "Void function cannot return a value"
    )

    reject_case(
            duplicate_local
            "import <stdlib>\nfunc main() -> void { var x:int; var x:int; }"
            "Duplicate variable"
    )

    reject_case(
            duplicate_function
            "import <stdlib>\nfunc main() -> void {} func main() -> void {}"
            "main cannot be overloaded"
    )

    reject_case(
            scope_escape
            "import <stdlib>\nfunc main() -> void { { var x:int=2; } println(x); }"
            "Undefined variable"
    )

    reject_case(
            loop_escape
            "import <stdlib>\nfunc main() -> void { for (var i:int=0;i<1;i++) {} println(i); }"
            "Undefined variable"
    )

    reject_case(
            break_outside
            "import <stdlib>\nfunc main() -> void { break; }"
            "outside a loop"
    )

    reject_case(
            invalid_lvalue
            "import <stdlib>\nfunc main() -> void { 1=2; }"
            "Assignment requires"
    )

    reject_case(
            float_remainder
            "import <stdlib>\nfunc main() -> void { println(1.5%2.0); }"
            "Remainder requires integer"
    )

    reject_case(
            pointer_mismatch
            "import <stdlib>\nfunc main() -> void { var x:int; var p:*double=&x; }"
            "Cannot implicitly convert"
    )

    reject_case(
            index_bounds
            "import <stdlib>\nfunc main() -> void { var a:int[3]; println(a[3]); }"
            "outside declared bounds"
    )

    reject_case(
            unknown_character
            "import <stdlib>\nfunc main() -> void {} $"
            "Unknown character"
    )

    reject_case(
            trailing_syntax
            "import <stdlib>\nfunc main() -> void {} +"
            "Expected function declaration"
    )

    reject_case(
            empty_character
            "import <stdlib>\nfunc main() -> void { var c:char=''; }"
            "exactly one byte"
    )

    reject_case(
            bad_exponent
            "import <stdlib>\nfunc main() -> void { var x:double=1e; }"
            "Invalid floating-point literal"
    )

    reject_case(
            integer_range
            "import <stdlib>\nfunc main() -> void { var x:u64=18446744073709551616; }"
            "outside unsigned 64-bit range"
    )

    reject_case(
            bit_arithmetic
            "import <stdlib>\nfunc main() -> void { var b:bit=true; b++; }"
            "Boolean values do not support"
    )

    reject_case(
            reserved_name
            "import <stdlib>\nfunc main() -> void { var true:int=3; }"
            "cannot be declared"
    )

    string(REPEAT "a" 600 long_text)

    reject_case(
            long_string
            "import <stdlib>\nfunc main() -> void { println(\"${long_text}\"); }"
            "Token exceeds maximum length"
    )

    string(REPEAT "1+" 300 long_expression)

    reject_case(
            deep_expression
            "import <stdlib>\nfunc main() -> void { println(${long_expression}1); }"
            "Expression tree exceeds"
    )

endif()


# ---------------------------------------------------------------------------
# Backend tests
# ---------------------------------------------------------------------------

if(TEST_STAGE STREQUAL "all" OR TEST_STAGE STREQUAL "backend")

    # This fits the code buffer but exceeds the optimizer's former
    # 10,000-line limit.
    string(REPEAT "println(1);\n" 1700 body)
    string(REPEAT "1\n" 1700 expected)

    dmm_test_write(
            "${OUTPUT_DIR}/many_lines/many_lines.dmm"
            "import <stdlib>\nfunc main() -> void {\n${body}}"
    )

    run_case(
            "${OUTPUT_DIR}/many_lines/many_lines.dmm"
            "${expected}"
            intel
    )

    # Cross-target assembly must use the requested target's
    # argument registers.
    foreach(target elf coff)

        dmm_test_write(
                "${OUTPUT_DIR}/target/target.dmm"
                "import <stdlib>\nfunc main() -> void { println(7); }"
        )

        execute_process(
                COMMAND "${COMPILER}" --emit=asm
                "--target=${target}"
                --syntax=att
                "${OUTPUT_DIR}/target/target.dmm"
                RESULT_VARIABLE result
                ERROR_VARIABLE errors
                TIMEOUT 30
        )

        file(
                READ
                "${OUTPUT_DIR}/target/target.dmm.s"
                assembly
        )

        # The typed emitter currently reserves only RBX. Callee-saved registers
        # that are never touched do not need artificial save/restore pairs.
        set(nonvolatile_registers rbx)
        foreach(nonvolatile IN LISTS nonvolatile_registers)
            string(FIND "${assembly}" "pushq %${nonvolatile}" saved_register)
            string(FIND "${assembly}" "popq %${nonvolatile}" restored_register)
            if(saved_register EQUAL -1 OR restored_register EQUAL -1)
                message(FATAL_ERROR
                        "${target} backend does not preserve non-volatile ${nonvolatile}")
            endif()
        endforeach()

        if(target STREQUAL "elf")
            set(register "%rsi")
            string(FIND "${assembly}" "call __dmm_core_snprintf" format_marker)
            if(format_marker EQUAL -1)
                message(FATAL_ERROR "ELF backend bypasses the standalone formatter")
            endif()
        else()
            set(register "%rdx")
        endif()

        string(
                FIND
                "${assembly}"
                ", ${register}"
                found
        )

        if(
                NOT result STREQUAL "0"
                OR found EQUAL -1
        )
            message(FATAL_ERROR
                    "Wrong calling convention for ${target}: ${errors}"
            )
        endif()

        message(STATUS "PASS target/${target}")

    endforeach()


    # Runtime-failure programs must compile in both syntaxes and then
    # terminate non-zero promptly.
    file(
            GLOB_RECURSE RUNTIME_FAILURE_FILES
            "${ROOT}/tests/runtime_failure/*.dmm"
    )

    foreach(source IN LISTS RUNTIME_FAILURE_FILES)

        get_filename_component(name "${source}" NAME_WE)

        foreach(syntax intel att)

            set(
                    work
                    "${OUTPUT_DIR}/runtime_${name}_${syntax}"
            )

            file(MAKE_DIRECTORY "${work}")

            file(
                    COPY_FILE
                    "${source}"
                    "${work}/input.dmm"
            )

            execute_process(
                    COMMAND "${COMPILER}" --emit=asm
                    "--syntax=${syntax}"
                    "${work}/input.dmm"
                    RESULT_VARIABLE result
                    ERROR_VARIABLE errors
                    TIMEOUT 30
            )

            if(NOT result STREQUAL "0")
                message(FATAL_ERROR
                        "${name}/${syntax}: runtime-failure source "
                        "did not compile: ${errors}"
                )
            endif()

            execute_process(
                    COMMAND "${ASSEMBLER}"
                    ${STANDALONE_FLAGS}
                    "${work}/input.dmm.s"
                    ${SYSTEM_LIBRARIES} -o "${work}/program.exe"
                    RESULT_VARIABLE result
                    ERROR_VARIABLE errors
                    TIMEOUT 30
            )

            if(NOT result STREQUAL "0")
                message(FATAL_ERROR
                        "${name}/${syntax}: runtime-failure source "
                        "did not assemble: ${errors}"
                )
            endif()

            execute_process(
                    COMMAND "${work}/program.exe"
                    RESULT_VARIABLE result
                    OUTPUT_VARIABLE output
                    ERROR_VARIABLE errors
                    TIMEOUT 10
            )

            if(
                    result STREQUAL "0"
                    OR result MATCHES "timeout"
            )
                message(FATAL_ERROR
                        "${name}/${syntax}: expected prompt non-zero "
                        "runtime failure, got ${result}"
                )
            endif()

            message(
                    STATUS
                    "PASS runtime-failure/${name}/${syntax}"
            )

        endforeach()

    endforeach()

endif()
