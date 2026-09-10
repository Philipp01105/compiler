cmake_minimum_required(VERSION 3.21)
project(compiler VERSION 5.1.0 LANGUAGES C)

add_compile_definitions(DMM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DMM_STRICT_WARNINGS "Treat compiler warnings as errors" OFF)
option(DMM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DMM_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

set(DMM_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ast
        ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/sema
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ir
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/x86_64
        ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
        ${CMAKE_CURRENT_SOURCE_DIR}/src/driver)

add_executable(compiler
        src/common/language_types.h
        src/common/token.h
        src/ast/ast.c
        src/ast/ast.h
        src/frontend/frontend.c
        src/frontend/frontend.h
        src/frontend/lexer.c
        src/frontend/lexer.h
        src/frontend/syntax_parser.c
        src/frontend/syntax_parser.h
        src/sema/semantic.c
        src/sema/semantic.h
        src/ir/ir.c
        src/ir/ir.h
        src/backend/backend.c
        src/backend/backend.h
        src/backend/x86_64/ir_emitter.c
        src/backend/x86_64/ir_emitter.h
        src/backend/x86_64/instruction.c
        src/backend/x86_64/instruction.h
        src/backend/asm_optimizer.c
        src/backend/asm_optimizer.h
        src/backend/syntax_converter.c
        src/backend/syntax_converter.h
        src/diagnostics/errorHandler.c
        src/diagnostics/errorHandler.h
        src/driver/main.c
)

target_include_directories(compiler PRIVATE ${DMM_INCLUDE_DIRS})
target_compile_definitions(compiler PRIVATE
        DMM_VERSION="${PROJECT_VERSION}")
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
install(DIRECTORY stdlib/ DESTINATION share/dmm/stdlib)

if(DMM_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DMM_BUILD_FUZZERS requires Clang")
    endif()
    add_executable(fuzz_syntax_converter tests/fuzz/fuzz_syntax_converter.c src/backend/syntax_converter.c)
    target_include_directories(fuzz_syntax_converter PRIVATE ${DMM_INCLUDE_DIRS})
    target_compile_options(fuzz_syntax_converter PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_syntax_converter PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_lexer tests/fuzz/fuzz_lexer.c src/frontend/lexer.c src/diagnostics/errorHandler.c)
    target_include_directories(fuzz_lexer PRIVATE ${DMM_INCLUDE_DIRS})
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

    add_executable(dmm_optimizer_test tests/unit/optimizer_test.c src/backend/asm_optimizer.c)
    target_include_directories(dmm_optimizer_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME optimizer_unit COMMAND dmm_optimizer_test
             ${CMAKE_BINARY_DIR}/optimizer_test_input.s)
    set_tests_properties(optimizer_unit PROPERTIES LABELS "unit;safety")

    add_executable(dmm_syntax_converter_test tests/unit/syntax_converter_test.c src/backend/syntax_converter.c)
    target_include_directories(dmm_syntax_converter_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME syntax_converter_unit COMMAND dmm_syntax_converter_test)
    set_tests_properties(syntax_converter_unit PROPERTIES LABELS "unit;safety")

    add_executable(dmm_x86_instruction_test
            tests/unit/x86_instruction_test.c src/backend/x86_64/instruction.c)
    target_include_directories(dmm_x86_instruction_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME x86_instruction_unit COMMAND dmm_x86_instruction_test)
    set_tests_properties(x86_instruction_unit PROPERTIES LABELS "unit;backend;safety")

    add_executable(dmm_lexer_test tests/unit/lexer_test.c src/frontend/lexer.c src/diagnostics/errorHandler.c)
    target_include_directories(dmm_lexer_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME lexer_unit COMMAND dmm_lexer_test)
    set_tests_properties(lexer_unit PROPERTIES LABELS "unit;lexer;safety")

    add_executable(dmm_frontend_ast_test
            tests/unit/frontend_ast_test.c
            src/ast/ast.c src/frontend/frontend.c src/frontend/syntax_parser.c
            src/frontend/lexer.c src/diagnostics/errorHandler.c)
    target_include_directories(dmm_frontend_ast_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME frontend_ast_unit COMMAND dmm_frontend_ast_test
             ${CMAKE_SOURCE_DIR}/tests/unit/frontend_ast_fixture.dmm)
    set_tests_properties(frontend_ast_unit PROPERTIES LABELS "unit;frontend;ast;safety")

    add_executable(dmm_pipeline_test
            tests/unit/pipeline_test.c src/ast/ast.c src/frontend/frontend.c
            src/frontend/syntax_parser.c src/frontend/lexer.c src/sema/semantic.c
            src/ir/ir.c src/diagnostics/errorHandler.c)
    target_include_directories(dmm_pipeline_test PRIVATE ${DMM_INCLUDE_DIRS})
    file(GLOB_RECURSE DMM_FRONTEND_CORPUS CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/execution/*.dmm)
    file(GLOB DMM_EXAMPLE_CORPUS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/examples/*.dmm)
    add_test(NAME frontend_pipeline_unit COMMAND dmm_pipeline_test
             ${CMAKE_SOURCE_DIR}/tests/unit/frontend_ast_fixture.dmm
             ${CMAKE_SOURCE_DIR}/tests/unit/import_root.dmm
             ${DMM_FRONTEND_CORPUS} ${DMM_EXAMPLE_CORPUS})
    set_tests_properties(frontend_pipeline_unit PROPERTIES LABELS "unit;frontend;ast;sema;ir;safety")

    add_test(NAME architecture_boundaries COMMAND ${CMAKE_COMMAND}
             -P ${CMAKE_SOURCE_DIR}/tests/architecture_test.cmake)
    set_tests_properties(architecture_boundaries PROPERTIES LABELS "unit;architecture;safety")

    add_test(NAME ir_backend_native COMMAND ${CMAKE_COMMAND}
             -DCOMPILER=$<TARGET_FILE:compiler>
             -DASSEMBLER=${DMM_ASSEMBLER}
             -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ir_backend_test
             -P ${CMAKE_SOURCE_DIR}/tests/ir_backend_test.cmake)
    set_tests_properties(ir_backend_native PROPERTIES LABELS "integration;backend;ir;safety")

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
        foreach(test_target dmm_optimizer_test dmm_syntax_converter_test dmm_x86_instruction_test dmm_lexer_test
                            dmm_frontend_ast_test dmm_pipeline_test)
            target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()
endif()
