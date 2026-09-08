cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED COMPILER OR NOT DEFINED ASSEMBLER OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "COMPILER, ASSEMBLER, and OUTPUT_DIR are required")
endif()
if(NOT DEFINED SANITIZER_FLAGS)
    set(SANITIZER_FLAGS "")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/math.dmm" "func square(n:int) -> int { return n * n; }\n")
file(WRITE "${OUTPUT_DIR}/types.dmm"
    "struct Point { var x:int; }\nenum State(code:int) { Ready(7), Done(9) }\n")
file(WRITE "${OUTPUT_DIR}/nested.dmm"
    "#import \"math.dmm\"\n#import \"types.dmm\"\nfunc answer() -> int { return square(6); }\n")
file(WRITE "${OUTPUT_DIR}/main.dmm"
    "#import \"nested.dmm\"\n#import \"nested.dmm\"\nfunc main() -> void { var p:Point; p.x=answer(); println(p.x); print(State.Ready.code); println(\"\"); }\n")

foreach(syntax intel att)
    set(assembly "${OUTPUT_DIR}/main_${syntax}.s")
    set(program "${OUTPUT_DIR}/main_${syntax}.exe")
    execute_process(COMMAND "${COMPILER}" --deterministic "--syntax=${syntax}" -o "${assembly}" "${OUTPUT_DIR}/main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Import compile failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${ASSEMBLER}" ${SANITIZER_FLAGS} -no-pie "${assembly}" -o "${program}"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Import assembly failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${program}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT result EQUAL 0 OR NOT output STREQUAL "36\n7\n")
        message(FATAL_ERROR "Imported function produced '${output}' (${syntax}): ${errors}")
    endif()
endforeach()

# A cycle is finite because each canonical file is processed only once.
file(WRITE "${OUTPUT_DIR}/cycle_a.dmm"
    "#import \"cycle_b.dmm\"\nfunc from_a() -> int { return 20; }\n")
file(WRITE "${OUTPUT_DIR}/cycle_b.dmm"
    "#import \"cycle_a.dmm\"\nfunc from_b() -> int { return 22; }\n")
file(WRITE "${OUTPUT_DIR}/cycle_main.dmm"
    "#import \"cycle_a.dmm\"\nfunc main() -> void { var total:int=from_a()+from_b(); println(total); }\n")
foreach(syntax intel att)
    set(assembly "${OUTPUT_DIR}/cycle_${syntax}.s")
    set(program "${OUTPUT_DIR}/cycle_${syntax}.exe")
    execute_process(COMMAND "${COMPILER}" --deterministic "--syntax=${syntax}" -o "${assembly}" "${OUTPUT_DIR}/cycle_main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cyclic import compile failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${ASSEMBLER}" ${SANITIZER_FLAGS} -no-pie "${assembly}" -o "${program}"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    execute_process(COMMAND "${program}" RESULT_VARIABLE run_result OUTPUT_VARIABLE output ERROR_VARIABLE run_errors TIMEOUT 10)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT result EQUAL 0 OR NOT run_result EQUAL 0 OR NOT output STREQUAL "42\n")
        message(FATAL_ERROR "Cyclic import execution failed (${syntax}): ${errors}${run_errors}")
    endif()
endforeach()

file(WRITE "${OUTPUT_DIR}/missing.dmm" "#import \"does-not-exist.dmm\"\nfunc main() -> void {}\n")
execute_process(COMMAND "${COMPILER}" --formatError "${OUTPUT_DIR}/missing.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics)
if(result EQUAL 0 OR NOT diagnostics MATCHES "Failed to open import file")
    message(FATAL_ERROR "Missing import was not diagnosed: ${diagnostics}")
endif()

file(WRITE "${OUTPUT_DIR}/broken_import.dmm" "func broken( -> int { return 1; }\n")
file(WRITE "${OUTPUT_DIR}/imports_broken.dmm"
    "#import \"broken_import.dmm\"\nfunc main() -> void {}\n")
execute_process(COMMAND "${COMPILER}" --formatError "${OUTPUT_DIR}/imports_broken.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 30)
if(result EQUAL 0 OR NOT diagnostics MATCHES "errors")
    message(FATAL_ERROR "Broken imported source was not rejected: ${diagnostics}")
endif()
