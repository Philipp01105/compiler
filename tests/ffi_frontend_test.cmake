cmake_minimum_required(VERSION 3.21)
set(work "${OUTPUT_DIR}/ffi_frontend")
file(MAKE_DIRECTORY "${work}/bindings")
file(WRITE "${work}/dmm.manifest" "module ffi.test/project\ndmm 2026-09-22-dev\n")
file(WRITE "${work}/bindings/types.dmm"
    "package bindings; extern \"system\" from \"unavailable_test_library\" {\n"
    "pub struct Record { pub var tag:u8; pub var count:u32; pub var bytes:u8[3]; }\n"
    "pub struct Handle; pub func query(handle:*Handle) -> i32 = \"native_query\";\n"
    "func hidden() -> i32; }\n")
set(valid "package main; import \"ffi.test/project/bindings\";\nfunc main() -> int { return bindings.Record.size.(int); }\n")
file(WRITE "${work}/main.dmm" "${valid}")
foreach(target elf coff)
    execute_process(COMMAND "${COMPILER}" --ide "--target=${target}" --formatError
        --dump-ast "${work}/${target}.ast" "${work}/main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Native package IDE analysis failed (${target}): ${diagnostics}")
    endif()
    file(READ "${work}/${target}.ast" tree)
    if(NOT tree MATCHES "native=1" OR NOT tree MATCHES "native-name=\"native_query\"" OR
       NOT tree MATCHES "opaque=1" OR NOT tree MATCHES "visibility=public")
        message(FATAL_ERROR "Native AST metadata missing: ${tree}")
    endif()
    foreach(level 0 1)
        execute_process(COMMAND "${COMPILER}" "--target=${target}" "-O${level}" --emit=obj --formatError
            --dump-ir "${work}/${target}_${level}.ir" -o "${work}/${target}_${level}.o" "${work}/main.dmm"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Unused native imports must not require a library or native lowering: ${diagnostics}")
        endif()
        file(READ "${work}/${target}_${level}.ir" ir)
        if(NOT ir MATCHES "dmm-ir-v6" OR NOT ir MATCHES "target=${target}" OR
           NOT ir MATCHES "native-import" OR NOT ir MATCHES "size=12 alignment=4")
            message(FATAL_ERROR "Native IR metadata missing: ${ir}")
        endif()
    endforeach()
endforeach()

file(WRITE "${work}/main.dmm"
    "package main; import \"ffi.test/project/bindings\"; func main() -> int { return bindings.hidden().(int); }\n")
execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${work}/private.ast" "${work}/main.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
if(result EQUAL 0 OR NOT diagnostics MATCHES "hidden")
    message(FATAL_ERROR "Private native function escaped its package: ${diagnostics}")
endif()

file(WRITE "${work}/main.dmm"
    "package main; import \"ffi.test/project/bindings\"; func main() -> int { var h:*bindings.Handle; return bindings.query(h).(int); }\n")
execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${work}/call.ast" "${work}/main.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Direct imported native call must be analyzable: ${diagnostics}")
endif()
execute_process(COMMAND "${COMPILER}" --emit=obj --formatError -o "${work}/unsupported.o" "${work}/main.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Native object emission must not require the native library: ${diagnostics}")
endif()
file(WRITE "${work}/main.dmm" "${valid}")
