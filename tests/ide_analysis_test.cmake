include("${CMAKE_CURRENT_LIST_DIR}/source_fixture.cmake")
cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(source "${OUTPUT_DIR}/recovery/recovery.dmm")
set(ast "${OUTPUT_DIR}/recovery.ast")
dmm_test_write( "${source}.s" "keep-existing-output")
dmm_test_write( "${source}" "func main() -> int {\n    var before = 1\n\n    before += 1;\n    var broken = ;\n    before += 1;\n    var after = 2;\n    return after;\n}\n")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8 TIMEOUT 5)
string(JSON line GET "${diagnostics}" errors 0 line)
string(JSON column GET "${diagnostics}" errors 0 column)
string(JSON replacement GET "${diagnostics}" errors 0 fix replacement)
if(result EQUAL 0 OR NOT line EQUAL 3 OR NOT column EQUAL 19 OR NOT replacement STREQUAL ";")
    message(FATAL_ERROR "Missing semicolon location/fix: ${diagnostics}")
endif()
# Normal compilation clears stale assembly; IDE analysis must leave it alone.
dmm_test_write( "${source}.s" "keep-existing-output")
execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${ast}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8 TIMEOUT 5)
string(JSON count LENGTH "${diagnostics}" errors)
file(READ "${ast}" dump)
file(READ "${source}.s" assembly)
if(result EQUAL 0 OR count LESS 2 OR NOT dump MATCHES "name=\"before\"" OR
   NOT dump MATCHES "name=\"after\"" OR dump MATCHES "name=\"broken\"" OR
   NOT assembly STREQUAL "keep-existing-output")
    message(FATAL_ERROR "Recovery/assembly isolation failed: ${diagnostics}\n${dump}")
endif()
foreach(text IN ITEMS
    "func main() -> int { var value = 1; return value;"
    "func broken( { }\nfunc main() -> int { var value = 1; return value; }")
    dmm_test_write( "${source}" "${text}")
    execute_process(COMMAND "${COMPILER}" --ide --formatError --dump-ast "${ast}" "${source}"
        RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8 TIMEOUT 5)
    file(READ "${ast}" dump)
    if(result EQUAL 0 OR NOT dump MATCHES "name=\"value\"")
        message(FATAL_ERROR "Block/declaration recovery failed: ${diagnostics}\n${dump}")
    endif()
endforeach()

dmm_test_write( "${source}" "func main() -> void {}\n")
dmm_test_write( "${OUTPUT_DIR}/helper/helper.dmm" "package helper;\npub func helper() -> int { return 1; }\n")
set(buffer "${OUTPUT_DIR}/editor/editor.dmm")
dmm_test_write( "${buffer}" "import \"dmm.test/generated/helper\";\nfunc main() -> int {\nvar live = helper.helper()\nreturn live;\n}\n")
execute_process(COMMAND "${COMPILER}" --ide --ide-buffer "${buffer}" --formatError --dump-ast "${ast}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8 TIMEOUT 5)
string(JSON line GET "${diagnostics}" errors 0 line)
string(JSON context GET "${diagnostics}" errors 0 sourceLine)
string(JSON filename GET "${diagnostics}" errors 0 filename)
file(READ "${ast}" dump)
file(READ "${source}" original)
if(NOT result EQUAL 1 OR NOT line EQUAL 4 OR NOT context STREQUAL "var live = helper.helper()" OR
   NOT filename STREQUAL source OR NOT dump MATCHES "name=\"live\"" OR
   NOT original MATCHES "package main;.*func main\\(\\) -> void \\{\\}")
    message(FATAL_ERROR "Editor override lost path/imports/context or changed source: ${diagnostics}\n${dump}")
endif()
