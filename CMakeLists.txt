cmake_minimum_required(VERSION 3.21)
project(compiler VERSION 5.1.0 LANGUAGES C)

add_compile_definitions(DMM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DMM_STRICT_WARNINGS "Treat compiler warnings as errors" OFF)
option(DMM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DMM_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(DMM_ENABLE_PERF_TESTS "Register opt-in runtime performance benchmarks" OFF)

set(DMM_FUZZ_PIPELINE_SOURCES
        tests/fuzz/fuzz_pipeline.c
        src/common/string_interner.c
        src/ast/ast.c
        src/ast/ast_optimize.c
        src/frontend/frontend.c
        src/frontend/lexer.c
        src/frontend/syntax_parser.c
        src/sema/semantic.c
        src/sema/semantic_interfaces.c
        src/sema/semantic_constants.c
        src/sema/semantic_generics.c
        src/sema/semantic_diagnostics.c
        src/sema/semantic_layout.c
        src/sema/semantic_analysis.c
        src/sema/semantic_borrows.c
        src/sema/semantic_ownership.c
        src/sema/semantic_expressions.c
        src/sema/semantic_symbols.c
        src/ast/generics.c
        src/ir/ir_optimize.c
        src/ir/ir.c
        src/ir/ir_verify.c
        src/ir/ir_cfg.c
        src/ir/ir_dump.c
        src/diagnostics/errorHandler.c)

set(DMM_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ast
        ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/sema
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ir
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/x86_64
        ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
        ${CMAKE_CURRENT_SOURCE_DIR}/src/driver)

add_executable(compiler
        src/common/language_types.h
        src/common/string_interner.c
        src/common/string_interner.h
        src/common/path_identity.c
        src/common/path_identity.h
        src/common/token.h
        src/ast/ast.c
        src/ast/ast.h
        src/ast/ast_optimize.c
        src/ast/ast_optimize.h
        src/ast/ast_dump.c
        src/frontend/frontend.c
        src/frontend/frontend.h
        src/frontend/token_dump.c
        src/frontend/lexer.c
        src/frontend/lexer.h
        src/frontend/syntax_parser.c
        src/frontend/syntax_parser.h
        src/sema/semantic.c
        src/sema/semantic_interfaces.c
        src/sema/semantic_constants.c
        src/sema/semantic_generics.c
        src/sema/semantic_diagnostics.c
        src/sema/semantic_layout.c
        src/sema/semantic_analysis.c
        src/sema/semantic_borrows.c
        src/sema/semantic_ownership.c
        src/sema/semantic_expressions.c
        src/sema/semantic_symbols.c
        src/ast/generics.c
        src/sema/semantic.h
        src/ir/ir.c
        src/ir/ir_verify.c
        src/ir/ir.h
        src/ir/ir_cfg.c
        src/ir/ir_cfg.h
        src/ir/ir_dump.c
        src/ir/ir_optimize.c
        src/ir/ir_optimize.h
        src/backend/backend.c
        src/backend/native/object.c
        src/backend/native/encoder.c
        src/backend/native/linker.c
        src/runtime/native_runtime.c
        src/backend/backend.h
        src/backend/runtime_calls.c
        src/backend/runtime_calls.h
        src/backend/x86_64/ir_emitter.c
        src/backend/x86_64/ir_arithmetic.c
        src/backend/x86_64/ir_calls.c
        src/backend/x86_64/ir_output.c
        src/backend/x86_64/ir_names.c
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
        src/driver/external_link.c
)

target_include_directories(compiler PRIVATE ${DMM_INCLUDE_DIRS})
if (NOT MSVC)
    if (WIN32)
        set(DMM_SHIM_FORMAT coff)
    else ()
        set(DMM_SHIM_FORMAT elf)
    endif ()
    add_library(dmm_platform_shim OBJECT src/runtime/platform_bundle.c)
    target_compile_options(dmm_platform_shim PRIVATE -Wall -Wextra -Wpedantic)
    if (DMM_STRICT_WARNINGS)
        target_compile_options(dmm_platform_shim PRIVATE -Wconversion -Wshadow -Werror)
    endif ()
    target_compile_definitions(dmm_platform_shim PRIVATE _XOPEN_SOURCE=700)
    add_dependencies(compiler dmm_platform_shim)
    set_property(TARGET compiler APPEND PROPERTY LINK_DEPENDS "$<TARGET_OBJECTS:dmm_platform_shim>")
    add_custom_command(TARGET compiler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:compiler>/dmm-runtime/${DMM_SHIM_FORMAT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_OBJECTS:dmm_platform_shim>
            "$<TARGET_FILE_DIR:compiler>/dmm-runtime/${DMM_SHIM_FORMAT}/platform-shim.o"
        COMMAND_EXPAND_LISTS)
    install(FILES $<TARGET_OBJECTS:dmm_platform_shim>
        DESTINATION bin/dmm-runtime/${DMM_SHIM_FORMAT} RENAME platform-shim.o)
    add_library(dmm_network_shim OBJECT src/runtime/network_bundle.c)
    target_compile_options(dmm_network_shim PRIVATE -Wall -Wextra -Wpedantic)
    if (DMM_STRICT_WARNINGS)
        target_compile_options(dmm_network_shim PRIVATE -Wconversion -Wshadow -Werror)
    endif ()
    target_compile_definitions(dmm_network_shim PRIVATE _GNU_SOURCE _XOPEN_SOURCE=700)
    add_dependencies(compiler dmm_network_shim)
    set_property(TARGET compiler APPEND PROPERTY LINK_DEPENDS "$<TARGET_OBJECTS:dmm_network_shim>")
    add_custom_command(TARGET compiler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_OBJECTS:dmm_network_shim>
            "$<TARGET_FILE_DIR:compiler>/dmm-runtime/${DMM_SHIM_FORMAT}/network-shim.o"
        COMMAND_EXPAND_LISTS)
    install(FILES $<TARGET_OBJECTS:dmm_network_shim>
        DESTINATION bin/dmm-runtime/${DMM_SHIM_FORMAT} RENAME network-shim.o)
endif ()
add_custom_command(TARGET compiler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:compiler>" "$<TARGET_FILE_DIR:compiler>/dmm$<TARGET_FILE_SUFFIX:compiler>"
        COMMENT "Creating the dmm command")

target_compile_definitions(compiler PRIVATE
        DMM_VERSION="${PROJECT_VERSION}")
if (MSVC)
    target_compile_options(compiler PRIVATE /W4)
    if (DMM_STRICT_WARNINGS)
        target_compile_options(compiler PRIVATE /WX)
    endif ()
else ()
    target_compile_options(compiler PRIVATE -Wall -Wextra -Wpedantic)
    if (DMM_STRICT_WARNINGS)
        target_compile_options(compiler PRIVATE -Wconversion -Wshadow -Werror)
    endif ()
endif ()

# MinGW's default PE stack is smaller than the Unix default. Keep the parser's
# documented 512-node nesting boundary usable under both GCC and Clang.
if (WIN32 AND NOT MSVC)
    target_link_options(compiler PRIVATE -Wl,--stack,8388608)
endif ()

if (DMM_SANITIZERS)
    if (MSVC OR WIN32)
        message(FATAL_ERROR "DMM_SANITIZERS currently requires GCC or Clang on a Unix-like platform")
    endif ()
    target_compile_options(compiler PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(compiler PRIVATE -fsanitize=address,undefined)
endif ()

install(TARGETS compiler RUNTIME DESTINATION bin)
install(DIRECTORY stdlib/ DESTINATION share/dmm/stdlib)

if (DMM_BUILD_FUZZERS)
    if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DMM_BUILD_FUZZERS requires Clang")
    endif ()
    add_executable(fuzz_syntax_converter tests/fuzz/fuzz_syntax_converter.c src/backend/syntax_converter.c)
    target_include_directories(fuzz_syntax_converter PRIVATE ${DMM_INCLUDE_DIRS})
    target_compile_options(fuzz_syntax_converter PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_syntax_converter PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_lexer tests/fuzz/fuzz_lexer.c src/frontend/lexer.c
            src/common/string_interner.c src/diagnostics/errorHandler.c)
    target_include_directories(fuzz_lexer PRIVATE ${DMM_INCLUDE_DIRS})
    target_compile_options(fuzz_lexer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_lexer PRIVATE -fsanitize=fuzzer,address,undefined)

    foreach (fuzz_stage parser semantic ir)
        add_executable(fuzz_${fuzz_stage} tests/fuzz/fuzz_${fuzz_stage}.c
                ${DMM_FUZZ_PIPELINE_SOURCES})
        target_include_directories(fuzz_${fuzz_stage} PRIVATE
                ${DMM_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz)
        target_compile_options(fuzz_${fuzz_stage} PRIVATE
                -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(fuzz_${fuzz_stage} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach ()
endif ()

include(CTest)
if (BUILD_TESTING)
    if (TARGET dmm_platform_shim)
        find_package(Threads REQUIRED)
        add_executable(dmm_platform_shim_test tests/unit/platform_shim_test.c src/runtime/platform_shim.c)
        target_include_directories(dmm_platform_shim_test PRIVATE ${DMM_INCLUDE_DIRS})
        target_link_libraries(dmm_platform_shim_test PRIVATE Threads::Threads)
        target_compile_definitions(dmm_platform_shim_test PRIVATE _XOPEN_SOURCE=700)
        target_compile_options(dmm_platform_shim_test PRIVATE -Wall -Wextra -Wpedantic)
        if (DMM_STRICT_WARNINGS)
            target_compile_options(dmm_platform_shim_test PRIVATE -Wconversion -Wshadow -Werror)
        endif ()
        add_test(NAME platform_shim_unit COMMAND dmm_platform_shim_test)
        set_tests_properties(platform_shim_unit PROPERTIES LABELS "unit;runtime;platform;safety" TIMEOUT 20)
        add_executable(dmm_network_shim_test tests/unit/network_shim_test.c
            src/runtime/network_shim.c src/runtime/platform_shim.c)
        target_include_directories(dmm_network_shim_test PRIVATE ${DMM_INCLUDE_DIRS})
        target_compile_definitions(dmm_network_shim_test PRIVATE _GNU_SOURCE _XOPEN_SOURCE=700 DMM_NET_TESTING)
        target_compile_options(dmm_network_shim_test PRIVATE -Wall -Wextra -Wpedantic)
        if(DMM_STRICT_WARNINGS)
            target_compile_options(dmm_network_shim_test PRIVATE -Wconversion -Wshadow -Werror)
        endif()
        target_link_libraries(dmm_network_shim_test PRIVATE Threads::Threads)
        if(WIN32)
            target_link_libraries(dmm_network_shim_test PRIVATE ws2_32)
        endif()
        add_test(NAME network_shim_unit COMMAND dmm_network_shim_test)
        set_tests_properties(network_shim_unit PROPERTIES LABELS "unit;runtime;network;safety" TIMEOUT 30)
        add_test(NAME network_runtime_contract COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler> -DASSEMBLER=${CMAKE_C_COMPILER}
            -DSHIM=$<TARGET_FILE_DIR:compiler>/dmm-runtime/${DMM_SHIM_FORMAT}/network-shim.o
            -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/network_runtime_test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_runtime_test.cmake)
        set_tests_properties(network_runtime_contract PROPERTIES LABELS "integration;runtime;network;safety;abi" TIMEOUT 180)
        find_program(DMM_PLATFORM_LINKER NAMES gcc REQUIRED)
        add_executable(dmm_linker_failure_driver tests/unit/linker_failure_driver.c)
        add_test(NAME platform_handoff_contract COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DASSEMBLER=${DMM_PLATFORM_LINKER}
            -DSHIM=$<TARGET_FILE_DIR:compiler>/dmm-runtime/${DMM_SHIM_FORMAT}/platform-shim.o
            -DFAIL_DRIVER=$<TARGET_FILE:dmm_linker_failure_driver>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/platform_handoff_test
            -P ${CMAKE_SOURCE_DIR}/tests/platform_handoff_test.cmake)
        set_tests_properties(platform_handoff_contract PROPERTIES LABELS "integration;platform;runtime;cli;safety" TIMEOUT 120)
    endif ()
    if (DMM_ENABLE_PERF_TESTS)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME runtime_performance
                COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/performance/run.py
                --compiler $<TARGET_FILE:compiler>
                --output-dir ${CMAKE_BINARY_DIR}/performance)
        set_tests_properties(runtime_performance PROPERTIES
                LABELS "performance;benchmark"
                RUN_SERIAL TRUE
                TIMEOUT 600)
    endif ()
    if (UNIX)
        add_compile_definitions(_XOPEN_SOURCE=700)
    endif ()

    add_executable(dmm_foundation_test
            tests/unit/foundation_test.c
            ${DMM_FUZZ_PIPELINE_SOURCES})
    add_executable(dmm_async_semantic_test
            tests/unit/async_semantic_test.c
            ${DMM_FUZZ_PIPELINE_SOURCES})
    target_include_directories(dmm_async_semantic_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME async_semantic_unit COMMAND dmm_async_semantic_test)
    set_tests_properties(async_semantic_unit PROPERTIES LABELS "unit;frontend;sema;async;safety")
    find_package(Threads REQUIRED)
    add_executable(dmm_executor_test tests/unit/executor_test.c src/runtime/executor.c)
    target_include_directories(dmm_executor_test PRIVATE ${DMM_INCLUDE_DIRS})
    target_link_libraries(dmm_executor_test PRIVATE Threads::Threads)
    add_test(NAME executor_runtime_unit COMMAND dmm_executor_test)
    set_tests_properties(executor_runtime_unit PROPERTIES LABELS "unit;runtime;async;safety" TIMEOUT 45)
    add_test(NAME async_frontend_contract
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/async_frontend_test
            -P ${CMAKE_SOURCE_DIR}/tests/async_frontend_test.cmake)
    set_tests_properties(async_frontend_contract PROPERTIES LABELS "integration;frontend;sema;async;safety")
    target_include_directories(dmm_foundation_test PRIVATE
            ${DMM_INCLUDE_DIRS}
            ${CMAKE_SOURCE_DIR}/tests/fuzz)
    add_test(NAME language_foundation_unit COMMAND dmm_foundation_test)
    set_tests_properties(language_foundation_unit PROPERTIES
            LABELS "unit;frontend;sema;ir;safety")

    set_source_files_properties(tests/resource_fault.c
            PROPERTIES COMPILE_DEFINITIONS DMM_RESOURCE_IMPLEMENTATION)

    get_target_property(DMM_COMPILER_SOURCES compiler SOURCES)
    list(REMOVE_ITEM DMM_COMPILER_SOURCES src/driver/main.c)

    add_executable(dmm_resource_fault_test
            tests/unit/resource_fault_test.c
            tests/resource_fault.c
            ${DMM_COMPILER_SOURCES})
    target_include_directories(dmm_resource_fault_test PRIVATE
            ${DMM_INCLUDE_DIRS}
            ${CMAKE_SOURCE_DIR}/tests)

    if (MSVC)
        target_compile_options(dmm_resource_fault_test PRIVATE
                /FI${CMAKE_SOURCE_DIR}/tests/resource_fault.h)
    else ()
        target_compile_options(dmm_resource_fault_test PRIVATE
                -include ${CMAKE_SOURCE_DIR}/tests/resource_fault.h)
    endif ()

    add_test(NAME resource_fault_unit
            COMMAND dmm_resource_fault_test
            ${CMAKE_BINARY_DIR}/resource_fault_output)
    set_tests_properties(resource_fault_unit PROPERTIES
            LABELS "unit;backend;diagnostics;safety")

    set(DMM_ASSEMBLER "${CMAKE_C_COMPILER}" CACHE FILEPATH
            "C compiler used to assemble and link generated code")
    add_test(NAME async_runtime_contract COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler> -DASSEMBLER=${DMM_ASSEMBLER}
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/async_runtime_test
            -P ${CMAKE_SOURCE_DIR}/tests/async_runtime_test.cmake)
    set_tests_properties(async_runtime_contract PROPERTIES LABELS "integration;async;ir;backend;execution;safety")
    add_test(NAME async_executor_contract COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/async_executor_test
            -P ${CMAKE_SOURCE_DIR}/tests/async_executor_test.cmake)
    set_tests_properties(async_executor_contract PROPERTIES LABELS "integration;async;ir;backend;execution;safety" TIMEOUT 120)
    if (NOT EXISTS "${DMM_ASSEMBLER}")
        message(FATAL_ERROR
                "BUILD_TESTING is ON, but DMM_ASSEMBLER does not exist: ${DMM_ASSEMBLER}")
    endif ()

    set(DMM_TEST_LINK_FLAGS "")
    if (DMM_SANITIZERS)
        set(DMM_TEST_LINK_FLAGS "-fsanitize=address,undefined")
    endif ()

    add_executable(dmm_runtime_test
            tests/unit/runtime_test.c
            src/runtime/native_runtime.c
            src/backend/native/encoder.c
            src/backend/native/object.c
            src/backend/x86_64/instruction.c)
    target_include_directories(dmm_runtime_test PRIVATE ${DMM_INCLUDE_DIRS})

    add_executable(dmm_native_test
            tests/unit/native_test.c
            src/backend/native/object.c
            src/backend/native/encoder.c
            src/backend/native/linker.c
            src/runtime/native_runtime.c
            src/backend/x86_64/instruction.c)
    target_include_directories(dmm_native_test PRIVATE ${DMM_INCLUDE_DIRS})

    add_test(NAME native_binary_unit COMMAND dmm_native_test)
    set_tests_properties(native_binary_unit PROPERTIES
            LABELS "unit;native;backend;safety")

    add_test(NAME runtime_unit
            COMMAND dmm_runtime_test ${CMAKE_BINARY_DIR}/runtime_test.dat)
    set_tests_properties(runtime_unit PROPERTIES
            LABELS "unit;runtime;safety" TIMEOUT 45)

    function(add_dmm_regression_stage stage label)
        add_test(NAME regression_${stage}
                COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=$<TARGET_FILE:compiler>
                -DASSEMBLER=${DMM_ASSEMBLER}
                "-DSANITIZER_FLAGS=${DMM_TEST_LINK_FLAGS}"
                -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/test_output/${stage}
                -DTEST_STAGE=${stage}
                -P ${CMAKE_SOURCE_DIR}/tests/run_tests.cmake)
        set_tests_properties(regression_${stage}
                PROPERTIES LABELS "integration;${label}")
    endfunction()

    add_dmm_regression_stage(positive execution)
    add_dmm_regression_stage(abi abi)
    add_dmm_regression_stage(rejection diagnostics)
    add_dmm_regression_stage(backend safety)

    add_test(NAME direct_native_backend
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DASSEMBLER=${DMM_ASSEMBLER}
            "-DSANITIZER_FLAGS=${DMM_TEST_LINK_FLAGS}"
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/direct_native_test
            -P ${CMAKE_SOURCE_DIR}/tests/native_backend_test.cmake)
    set_tests_properties(direct_native_backend PROPERTIES
            LABELS "integration;backend;native;safety")

    add_test(NAME core_runtime_contract
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DASSEMBLER=${DMM_ASSEMBLER}
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/core_runtime_test
            -P ${CMAKE_SOURCE_DIR}/tests/core_runtime_test.cmake)
    set_tests_properties(core_runtime_contract PROPERTIES
            LABELS "integration;core;runtime;execution;abi;safety")

    add_test(NAME native_cli_contract
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/native_cli_test
            -P ${CMAKE_SOURCE_DIR}/tests/native_cli_test.cmake)
    set_tests_properties(native_cli_contract PROPERTIES
            LABELS "integration;cli;native;safety")

    if (DMM_SANITIZERS)
        set_property(TEST regression_positive APPEND PROPERTY LABELS safety)
        set_property(TEST regression_abi APPEND PROPERTY LABELS safety)
    endif ()

    add_executable(dmm_optimizer_test
            tests/unit/optimizer_test.c
            src/backend/asm_optimizer.c
            src/backend/backend.c
            src/diagnostics/errorHandler.c)
    target_compile_definitions(dmm_optimizer_test PRIVATE DMM_OPTIMIZER_FAULT_TEST)
    target_include_directories(dmm_optimizer_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME optimizer_unit
            COMMAND dmm_optimizer_test
            ${CMAKE_BINARY_DIR}/optimizer_test_input.s)
    set_tests_properties(optimizer_unit PROPERTIES LABELS "unit;safety")

    add_executable(dmm_syntax_converter_test
            tests/unit/syntax_converter_test.c
            src/backend/syntax_converter.c)
    target_include_directories(dmm_syntax_converter_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME syntax_converter_unit COMMAND dmm_syntax_converter_test)
    set_tests_properties(syntax_converter_unit PROPERTIES LABELS "unit;safety")

    add_executable(dmm_x86_instruction_test
            tests/unit/x86_instruction_test.c
            src/backend/x86_64/instruction.c)
    target_include_directories(dmm_x86_instruction_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME x86_instruction_unit COMMAND dmm_x86_instruction_test)
    set_tests_properties(x86_instruction_unit PROPERTIES
            LABELS "unit;backend;safety")

    add_executable(dmm_string_interner_test
            tests/unit/string_interner_test.c
            src/common/string_interner.c)
    target_include_directories(dmm_string_interner_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME string_interner_unit COMMAND dmm_string_interner_test)
    set_tests_properties(string_interner_unit PROPERTIES
            LABELS "unit;frontend;safety")

    add_executable(dmm_fuzz_pipeline_smoke_test
            tests/unit/fuzz_pipeline_smoke_test.c
            ${DMM_FUZZ_PIPELINE_SOURCES})
    target_include_directories(dmm_fuzz_pipeline_smoke_test PRIVATE
            ${DMM_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz)
    add_test(NAME fuzz_pipeline_smoke COMMAND dmm_fuzz_pipeline_smoke_test)
    set_tests_properties(fuzz_pipeline_smoke PROPERTIES
            LABELS "unit;fuzz;parser;sema;ir;safety")

    add_executable(dmm_lexer_test
            tests/unit/lexer_test.c
            src/frontend/lexer.c
            src/common/string_interner.c
            src/diagnostics/errorHandler.c)
    target_include_directories(dmm_lexer_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME lexer_unit COMMAND dmm_lexer_test)
    set_tests_properties(lexer_unit PROPERTIES LABELS "unit;lexer;safety")

    add_executable(dmm_frontend_ast_test
            tests/unit/frontend_ast_test.c
            src/ast/ast.c
            src/frontend/frontend.c
            src/frontend/syntax_parser.c
            src/frontend/lexer.c
            src/common/string_interner.c
            src/diagnostics/errorHandler.c)
    target_include_directories(dmm_frontend_ast_test PRIVATE ${DMM_INCLUDE_DIRS})
    add_test(NAME frontend_ast_unit
            COMMAND dmm_frontend_ast_test
            ${CMAKE_SOURCE_DIR}/tests/unit/frontend_ast_fixture/frontend_ast_fixture.dmm)
    set_tests_properties(frontend_ast_unit PROPERTIES
            LABELS "unit;frontend;ast;safety")

    add_executable(dmm_pipeline_test
            tests/unit/pipeline_test.c
            src/ast/ast.c
            src/frontend/frontend.c
            src/frontend/syntax_parser.c
            src/frontend/lexer.c
            src/sema/semantic.c
            src/sema/semantic_interfaces.c
            src/sema/semantic_constants.c
            src/sema/semantic_generics.c
            src/sema/semantic_diagnostics.c
            src/sema/semantic_layout.c
            src/sema/semantic_analysis.c
            src/sema/semantic_borrows.c
            src/sema/semantic_ownership.c
            src/sema/semantic_expressions.c
            src/sema/semantic_symbols.c
            src/ast/generics.c
            src/ir/ir.c
            src/ir/ir_verify.c
            src/ir/ir_cfg.c
            src/common/string_interner.c
            src/diagnostics/errorHandler.c)
    target_include_directories(dmm_pipeline_test PRIVATE ${DMM_INCLUDE_DIRS})

    add_executable(dmm_ir_optimizer_test
            tests/unit/ir_optimizer_test.c
            src/ast/ast.c
            src/ast/ast_optimize.c
            src/frontend/frontend.c
            src/frontend/syntax_parser.c
            src/frontend/lexer.c
            src/sema/semantic.c
            src/sema/semantic_interfaces.c
            src/sema/semantic_constants.c
            src/sema/semantic_generics.c
            src/sema/semantic_diagnostics.c
            src/sema/semantic_layout.c
            src/sema/semantic_analysis.c
            src/sema/semantic_borrows.c
            src/sema/semantic_ownership.c
            src/sema/semantic_expressions.c
            src/sema/semantic_symbols.c
            src/ast/generics.c
            src/ir/ir.c
            src/ir/ir_verify.c
            src/ir/ir_cfg.c
            src/ir/ir_dump.c
            src/ir/ir_optimize.c
            src/common/string_interner.c
            src/diagnostics/errorHandler.c)
    target_include_directories(dmm_ir_optimizer_test PRIVATE ${DMM_INCLUDE_DIRS})

    add_test(NAME ir_optimizer_unit COMMAND dmm_ir_optimizer_test)
    set_tests_properties(ir_optimizer_unit PROPERTIES
            LABELS "unit;ir;backend;safety")

    add_test(NAME ir_optimization_execution
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ir_optimization_test
            -P ${CMAKE_SOURCE_DIR}/tests/ir_optimization_test.cmake)
    set_tests_properties(ir_optimization_execution PROPERTIES
            LABELS "integration;ir;backend;execution;safety")

    file(GLOB_RECURSE DMM_FRONTEND_CORPUS CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/execution/*.dmm)
    file(GLOB_RECURSE DMM_EXAMPLE_CORPUS CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/examples/*.dmm)

    add_test(NAME frontend_pipeline_unit
            COMMAND dmm_pipeline_test
            ${CMAKE_SOURCE_DIR}/tests/unit/frontend_ast_fixture/frontend_ast_fixture.dmm
            ${CMAKE_SOURCE_DIR}/tests/unit/import_fixture/main.dmm
            ${CMAKE_SOURCE_DIR}/tests/unit/package_import_fixture/main.dmm
            ${DMM_FRONTEND_CORPUS}
            ${DMM_EXAMPLE_CORPUS})
    set_tests_properties(frontend_pipeline_unit PROPERTIES
            LABELS "unit;frontend;ast;sema;ir;safety")

    add_test(NAME architecture_boundaries
            COMMAND ${CMAKE_COMMAND}
            -P ${CMAKE_SOURCE_DIR}/tests/architecture_test.cmake)
    set_tests_properties(architecture_boundaries PROPERTIES
            LABELS "unit;architecture;safety")

    add_test(NAME ir_backend_native
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DASSEMBLER=${DMM_ASSEMBLER}
            "-DSANITIZER_FLAGS=${DMM_TEST_LINK_FLAGS}"
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ir_backend_test
            -P ${CMAKE_SOURCE_DIR}/tests/ir_backend_test.cmake)
    set_tests_properties(ir_backend_native PROPERTIES
            LABELS "integration;backend;ir;safety")

    add_test(NAME diagnostics_json
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/diagnostics_test
            -P ${CMAKE_SOURCE_DIR}/tests/diagnostics_test.cmake)
    set_tests_properties(diagnostics_json PROPERTIES
            LABELS "integration;diagnostics;safety")

    add_test(NAME ide_analysis
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ide_test
            -P ${CMAKE_SOURCE_DIR}/tests/ide_analysis_test.cmake)
    set_tests_properties(ide_analysis PROPERTIES
            LABELS "integration;diagnostics;frontend;safety")

    add_test(NAME diagnostics_audit
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/diagnostics_audit
            -P ${CMAKE_SOURCE_DIR}/tests/diagnostics_audit_test.cmake)
    set_tests_properties(diagnostics_audit PROPERTIES
            LABELS "integration;diagnostics;safety")

    add_test(NAME diagnostics_cases
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/diagnostics_cases
            -P ${CMAKE_SOURCE_DIR}/tests/diagnostics_cases_test.cmake)
    set_tests_properties(diagnostics_cases PROPERTIES
            LABELS "integration;diagnostics;safety")

    add_test(NAME cli_contract
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/cli_test
            -P ${CMAKE_SOURCE_DIR}/tests/cli_test.cmake)
    set_tests_properties(cli_contract PROPERTIES LABELS "integration;cli")

    add_test(NAME dump_formats
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/dump_test
            -P ${CMAKE_SOURCE_DIR}/tests/dump_test.cmake)
    set_tests_properties(dump_formats PROPERTIES
            LABELS "integration;ast;ir;backend;cli;safety")

    add_test(NAME imports_integration
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DASSEMBLER=${DMM_ASSEMBLER}
            "-DSANITIZER_FLAGS=${DMM_TEST_LINK_FLAGS}"
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/imports_test
            -P ${CMAKE_SOURCE_DIR}/tests/imports_test.cmake)
    set_tests_properties(imports_integration PROPERTIES LABELS "integration;imports")

    add_test(NAME manifest_commands
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/manifest_test
            -P ${CMAKE_SOURCE_DIR}/tests/manifest_test.cmake)
    set_tests_properties(manifest_commands PROPERTIES LABELS "integration;imports;cli")

    if (DMM_SANITIZERS)
        set_property(TEST imports_integration APPEND PROPERTY LABELS safety)
    endif ()

    add_test(NAME stress_limits
            COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:compiler>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/stress_test
            -P ${CMAKE_SOURCE_DIR}/tests/stress/limits_test.cmake)
    set_tests_properties(stress_limits PROPERTIES
            LABELS "integration;stress;safety")

    if (DMM_SANITIZERS)
        foreach (test_target
                dmm_platform_shim_test
                dmm_executor_test
                dmm_async_semantic_test
                dmm_foundation_test
                dmm_resource_fault_test
                dmm_optimizer_test
                dmm_syntax_converter_test
                dmm_x86_instruction_test
                dmm_string_interner_test
                dmm_fuzz_pipeline_smoke_test
                dmm_lexer_test
                dmm_frontend_ast_test
                dmm_pipeline_test
                dmm_runtime_test
                dmm_native_test
                dmm_ir_optimizer_test)
            target_compile_options(${test_target} PRIVATE
                    -fsanitize=address,undefined
                    -fno-omit-frame-pointer)
            target_link_options(${test_target} PRIVATE
                    -fsanitize=address,undefined)
        endforeach ()
    endif ()
endif ()
