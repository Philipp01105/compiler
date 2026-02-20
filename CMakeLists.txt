cmake_minimum_required(VERSION 3.21)
project(compiler VERSION 5.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DMM_STRICT_WARNINGS "Treat compiler warnings as errors" OFF)
option(DMM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DMM_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

add_executable(compiler
        src/lexer.c
        src/lexer.h
        src/compiler_types.h
        src/main.c
        src/parser_types.c
        src/parser_codegen.c
        src/parser_core.c
        src/parser_declarations.c
        src/parser_statements.c
        src/parser_expressions.c
        src/parser.h
        src/parser_internal.h
        src/errorHandler.c
        src/errorHandler.h
        src/asm_optimizer.c
        src/asm_optimizer.h
        src/instruction_builder.c
        src/instruction_builder.h
        src/syntax_converter.c
        src/syntax_converter.h
)

target_compile_definitions(compiler PRIVATE DMM_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(compiler PRIVATE /W4)
    if(DMM_STRICT_WARNINGS)
        target_compile_options(compiler PRIVATE /WX)
    endif()
else()
    target_compile_options(compiler PRIVATE -Wall -Wextra -Wpedantic)
    if(DMM_STRICT_WARNINGS)
        target_compile_options(compiler PRIVATE -Wconversion -Wshadow -Werror)
    endif()
endif()

if(DMM_SANITIZERS)
    if(MSVC OR WIN32)
        message(FATAL_ERROR "DMM_SANITIZERS currently requires GCC or Clang on a Unix-like platform")
    endif()
    target_compile_options(compiler PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(compiler PRIVATE -fsanitize=address,undefined)
endif()

install(TARGETS compiler RUNTIME DESTINATION bin)

if(DMM_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DMM_BUILD_FUZZERS requires Clang")
    endif()
    add_executable(fuzz_syntax_converter tests/fuzz/fuzz_syntax_converter.c src/syntax_converter.c)
    target_include_directories(fuzz_syntax_converter PRIVATE src)
    target_compile_options(fuzz_syntax_converter PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_syntax_converter PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_lexer tests/fuzz/fuzz_lexer.c src/lexer.c src/errorHandler.c)
    target_include_directories(fuzz_lexer PRIVATE src)
    target_compile_options(fuzz_lexer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_lexer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

include(CTest)
if(BUILD_TESTING)
    set(DMM_ASSEMBLER "${CMAKE_C_COMPILER}" CACHE FILEPATH
        "C compiler used to assemble and link generated code")
    if(NOT EXISTS "${DMM_ASSEMBLER}")
        message(FATAL_ERROR "BUILD_TESTING is ON, but DMM_ASSEMBLER does not exist: ${DMM_ASSEMBLER}")
    endif()
    set(DMM_TEST_LINK_FLAGS "")
    if(DMM_SANITIZERS)
        set(DMM_TEST_LINK_FLAGS "-fsanitize=address,undefined")
    endif()
    function(add_dmm_regression_stage stage label)
        add_test(NAME regression_${stage}
                COMMAND ${CMAKE_COMMAND}
                    -DCOMPILER=$<TARGET_FILE:compiler>
                    -DASSEMBLER=${DMM_ASSEMBLER}
                    "-DSANITIZER_FLAGS=${DMM_TEST_LINK_FLAGS}"
                    -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/test_output/${stage}
                    -DTEST_STAGE=${stage}
                    -P ${CMAKE_SOURCE_DIR}/tests/run_tests.cmake)
        set_tests_properties(regression_${stage} PROPERTIES LABELS "integration;${label}")
    endfunction()
    add_dmm_regression_stage(positive execution)
    add_dmm_regression_stage(abi abi)
    add_dmm_regression_stage(rejection diagnostics)
    add_dmm_regression_stage(backend safety)
    if(DMM_SANITIZERS)
        set_property(TEST regression_positive APPEND PROPERTY LABELS safety)
        set_property(TEST regression_abi APPEND PROPERTY LABELS safety)
    endif()

    add_executable(dmm_optimizer_test tests/unit/optimizer_test.c src/asm_optimizer.c)
    target_include_directories(dmm_optimizer_test PRIVATE src)
    add_test(NAME optimizer_unit COMMAND dmm_optimizer_test
             ${CMAKE_BINARY_DIR}/optimizer_test_input.s)
    set_tests_properties(optimizer_unit PROPERTIES LABELS "unit;safety")

    add_executable(dmm_syntax_converter_test tests/unit/syntax_converter_test.c src/syntax_converter.c)
    target_include_directories(dmm_syntax_converter_test PRIVATE src)
    add_test(NAME syntax_converter_unit COMMAND dmm_syntax_converter_test)
    set_tests_properties(syntax_converter_unit PROPERTIES LABELS "unit;safety")

    add_executable(dmm_lexer_test tests/unit/lexer_test.c src/lexer.c src/errorHandler.c)
    target_include_directories(dmm_lexer_test PRIVATE src)
    add_test(NAME lexer_unit COMMAND dmm_lexer_test)
    set_tests_properties(lexer_unit PROPERTIES LABELS "unit;lexer;safety")

    add_test(NAME diagnostics_json COMMAND ${CMAKE_COMMAND}
             -DCOMPILER=$<TARGET_FILE:compiler>
             -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/diagnostics_test
             -P ${CMAKE_SOURCE_DIR}/tests/diagnostics_test.cmake)
    set_tests_properties(diagnostics_json PROPERTIES LABELS "integration;diagnostics;safety")

    add_test(NAME cli_contract COMMAND ${CMAKE_COMMAND}
             -DCOMPILER=$<TARGET_FILE:compiler>
             -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/cli_test
             -P ${CMAKE_SOURCE_DIR}/tests/cli_test.cmake)
    set_tests_properties(cli_contract PROPERTIES LABELS "integration;cli")

    add_test(NAME imports_integration COMMAND ${CMAKE_COMMAND}
             -DCOMPILER=$<TARGET_FILE:compiler>
             -DASSEMBLER=${DMM_ASSEMBLER}
             "-DSANITIZER_FLAGS=${DMM_TEST_LINK_FLAGS}"
             -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/imports_test
             -P ${CMAKE_SOURCE_DIR}/tests/imports_test.cmake)
    set_tests_properties(imports_integration PROPERTIES LABELS "integration;imports")
    if(DMM_SANITIZERS)
        set_property(TEST imports_integration APPEND PROPERTY LABELS safety)
    endif()

    add_test(NAME stress_limits COMMAND ${CMAKE_COMMAND}
             -DCOMPILER=$<TARGET_FILE:compiler>
             -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/stress_test
             -P ${CMAKE_SOURCE_DIR}/tests/stress/limits_test.cmake)
    set_tests_properties(stress_limits PROPERTIES LABELS "integration;stress;safety")

    if(DMM_SANITIZERS)
        foreach(test_target dmm_optimizer_test dmm_syntax_converter_test dmm_lexer_test)
            target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()
endif()
