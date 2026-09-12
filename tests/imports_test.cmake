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
    "import \"math.dmm\"\nimport \"types.dmm\"\nfunc answer() -> int { return square(6); }\n")
file(WRITE "${OUTPUT_DIR}/main.dmm"
    "import \"nested.dmm\"\nimport \"nested.dmm\"\nimport <stdlib>\nfunc main() -> void { var p:Point; p.x=answer(); println(p.x); print(State.Ready.code); println(\"\"); }\n")

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
    "import \"cycle_b.dmm\"\nfunc from_a() -> int { return 20; }\n")
file(WRITE "${OUTPUT_DIR}/cycle_b.dmm"
    "import \"cycle_a.dmm\"\nfunc from_b() -> int { return 22; }\n")
file(WRITE "${OUTPUT_DIR}/cycle_main.dmm"
    "import \"cycle_a.dmm\"\nimport <stdlib>\nfunc main() -> void { var total:int=from_a()+from_b(); println(total); }\n")
foreach(syntax intel att)
    set(assembly "${OUTPUT_DIR}/cycle_${syntax}.s")
    set(program "${OUTPUT_DIR}/cycle_${syntax}.exe")
    execute_process(COMMAND "${COMPILER}" --deterministic "--syntax=${syntax}" -o "${assembly}" "${OUTPUT_DIR}/cycle_main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cyclic import compile failed (${syntax}): ${errors}")
    endif()
    file(READ "${assembly}" assembly_text)
    if(NOT assembly_text MATCHES "# Lowering: typed IR")
        message(FATAL_ERROR "Cyclic scalar imports did not use typed IR (${syntax})")
    endif()
    execute_process(COMMAND "${ASSEMBLER}" ${SANITIZER_FLAGS} -no-pie "${assembly}" -o "${program}"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    execute_process(COMMAND "${program}" RESULT_VARIABLE run_result OUTPUT_VARIABLE output ERROR_VARIABLE run_errors TIMEOUT 10)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT result EQUAL 0 OR NOT run_result EQUAL 0 OR NOT output STREQUAL "42\n")
        message(FATAL_ERROR "Cyclic import execution failed (${syntax}): ${errors}${run_errors}")
    endif()
endforeach()

file(WRITE "${OUTPUT_DIR}/missing.dmm" "import \"does-not-exist.dmm\"\nimport <stdlib>\nfunc main() -> void {}\n")
execute_process(COMMAND "${COMPILER}" --formatError "${OUTPUT_DIR}/missing.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics)
if(result EQUAL 0 OR NOT diagnostics MATCHES "Failed to open import file")
    message(FATAL_ERROR "Missing import was not diagnosed: ${diagnostics}")
endif()

file(WRITE "${OUTPUT_DIR}/broken_import.dmm" "func broken( -> int { return 1; }\n")
file(WRITE "${OUTPUT_DIR}/imports_broken.dmm"
    "import \"broken_import.dmm\"\nimport <stdlib>\nfunc main() -> void {}\n")
execute_process(COMMAND "${COMPILER}" --formatError "${OUTPUT_DIR}/imports_broken.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 30)
if(result EQUAL 0 OR NOT diagnostics MATCHES "errors")
    message(FATAL_ERROR "Broken imported source was not rejected: ${diagnostics}")
endif()

# Directory imports load only the manifest and its explicit dependencies.
file(MAKE_DIRECTORY "${OUTPUT_DIR}/pkg/internal" "${OUTPUT_DIR}/missing_manifest")
file(WRITE "${OUTPUT_DIR}/pkg/package.dmm" "import \"api.dmm\"\n")
file(WRITE "${OUTPUT_DIR}/pkg/api.dmm" "func package_answer() -> int { return 42; }\n")
file(WRITE "${OUTPUT_DIR}/pkg/internal/broken.dmm" "this must never be parsed\n")
file(WRITE "${OUTPUT_DIR}/pkg/unlisted.dmm" "this must never be parsed\n")
file(WRITE "${OUTPUT_DIR}/package_main.dmm"
    "import (\"pkg\" \"pkg/./package.dmm\" \"pkg/api.dmm\")\nfunc main() -> int { return package_answer()-42; }\n")
foreach(syntax intel att)
    set(assembly "${OUTPUT_DIR}/package_${syntax}.s")
    set(program "${OUTPUT_DIR}/package_${syntax}.exe")
    execute_process(COMMAND "${COMPILER}" "--syntax=${syntax}" -o "${assembly}" "${OUTPUT_DIR}/package_main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Package import failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${ASSEMBLER}" ${SANITIZER_FLAGS} -no-pie "${assembly}" -o "${program}"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Package assembly failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${program}" RESULT_VARIABLE result TIMEOUT 10)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Package execution failed (${syntax}): ${result}")
    endif()
endforeach()
file(WRITE "${OUTPUT_DIR}/missing_manifest/api.dmm" "func optional() -> int { return 1; }\n")
file(WRITE "${OUTPUT_DIR}/missing_package.dmm" "import \"missing_manifest\"\nfunc main() -> void {}\n")
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/missing_package.dmm"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics)
if(result EQUAL 0 OR NOT diagnostics MATCHES "package.dmm")
    message(FATAL_ERROR "Missing package manifest was not diagnosed: ${diagnostics}")
endif()

# Shared constant dependencies are evaluated once, before their importing units.
file(WRITE "${OUTPUT_DIR}/constant_base.dmm"
    "const imported_count=2+3;\nconst imported_text=\"from\"+\" import\";\n")
file(WRITE "${OUTPUT_DIR}/constant_left.dmm"
    "import \"constant_base.dmm\"\nconst twice=imported_count*2;\n")
file(WRITE "${OUTPUT_DIR}/constant_right.dmm"
    "import (\"constant_left.dmm\" \"constant_base.dmm\")\nconst combined=twice+imported_count;\n")
file(WRITE "${OUTPUT_DIR}/constant_main.dmm"
    "import (\"constant_left.dmm\" \"constant_right.dmm\")\nfunc main() -> int { var values:int[twice]; values[9]=combined; if (values[9]!=15) { return 1; } if (imported_text!=\"from import\") { return 2; } return 0; }\n")
foreach(syntax intel att)
    set(assembly "${OUTPUT_DIR}/constants_${syntax}.s")
    set(program "${OUTPUT_DIR}/constants_${syntax}.exe")
    execute_process(COMMAND "${COMPILER}" "--syntax=${syntax}" -o "${assembly}" "${OUTPUT_DIR}/constant_main.dmm"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Imported constants failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${ASSEMBLER}" ${SANITIZER_FLAGS} -no-pie "${assembly}" -o "${program}"
        RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Imported constants assembly failed (${syntax}): ${errors}")
    endif()
    execute_process(COMMAND "${program}" RESULT_VARIABLE result TIMEOUT 10)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Imported constants execution failed (${syntax}): ${result}")
    endif()
endforeach()
