cmake_minimum_required(VERSION 3.21)
set(work "${OUTPUT_DIR}/async_frontend")
file(MAKE_DIRECTORY "${work}/worker")
file(WRITE "${work}/dmm.manifest"
     "module async.test/project\ndmm 2026-09-22-dev\nfeatures = [\"async\"]\n")
file(WRITE "${work}/worker/worker.dmm"
     "package worker; pub async func value() -> int { return 7; }\n"
     "pub enum Result<T> { pub Value(T), } pub func customResult() -> Result<int> { return Result<int>.Value(5); }\n"
     "pub async func join(h:JoinHandle<int>) -> int { var r=h.await(); match(r) { Ok(v) => return v; Err(e) => return 0; } }\n"
     "pub async func array() -> int[2] { return [7,8]; }\n")
file(WRITE "${work}/main.dmm"
     "package main; import \"async.test/project/worker\";\n"
     "pub async func use() -> int { var f:Future<int>=worker.value(); var v=f.await(); var a=worker.array().await(); return v+a[1]; }\n"
     "func custom() -> int { var r:worker.Result<int>=worker.customResult(); match(r) { Value(v) => return v; } }\n"
     "func main() -> int { return 0; }\n")
execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${work}/tree.ast" "${work}/main.dmm"
                RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Async manifest/import typing failed: ${diagnostics}")
endif ()
file(READ "${work}/tree.ast" tree)
if (NOT tree MATCHES "async=1" OR NOT tree MATCHES "Future<int>")
    message(FATAL_ERROR "Async/Future AST metadata missing: ${tree}")
endif ()
foreach (level 0 1)
    foreach (target elf coff)
        execute_process(COMMAND "${COMPILER}" "-O${level}" "--target=${target}" --emit=obj --formatError
                        --dump-ir "${work}/async_${level}_${target}.ir" --dump-cfg "${work}/async_${level}_${target}.cfg"
                        "${work}/main.dmm" -o "${work}/async_${level}_${target}.obj"
                        RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
        if (NOT result EQUAL 0 OR NOT EXISTS "${work}/async_${level}_${target}.obj")
            message(FATAL_ERROR "Async native code generation failed: ${diagnostics}")
        endif ()
        file(READ "${work}/async_${level}_${target}.ir" ir)
        file(READ "${work}/async_${level}_${target}.cfg" cfg)
        if(NOT ir MATCHES "pinned=1" OR NOT ir MATCHES "opcode=await" OR NOT cfg MATCHES "await")
            message(FATAL_ERROR "Async IR/frame/CFG metadata missing for ${target}/O${level}")
        endif()
    endforeach ()
endforeach ()
file(READ "${work}/main.dmm" valid_source)
foreach(invalid "await worker.value()" "worker.value().await(1)")
    file(WRITE "${work}/main.dmm"
        "package main; import \"async.test/project/worker\"; async func use() -> int { return ${invalid}; } func main() -> int { return 0; }\n")
    execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${work}/invalid.ast" "${work}/main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
    if(result EQUAL 0 OR NOT diagnostics MATCHES "Prefix await was removed|Future.await\\(\\) takes no arguments")
        message(FATAL_ERROR "Invalid await syntax was accepted: ${invalid}: ${diagnostics}")
    endif()
endforeach()
file(WRITE "${work}/main.dmm" "${valid_source}")
file(WRITE "${work}/dmm.manifest" "module async.test/project\ndmm 2026-09-22-dev\nfeatures = []\n")
execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${work}/disabled.ast" "${work}/main.dmm"
                RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
if (result EQUAL 0 OR NOT diagnostics MATCHES "Future types require the async manifest feature")
    message(FATAL_ERROR "The root manifest must enable async: ${diagnostics}")
endif ()
