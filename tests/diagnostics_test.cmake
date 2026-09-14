include("${CMAKE_CURRENT_LIST_DIR}/source_fixture.cmake")
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED COMPILER OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "COMPILER and OUTPUT_DIR are required")
endif()

# A method reference must fail in sema, show its source, and offer a precise edit.
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(source "${OUTPUT_DIR}/invalid/invalid.dmm")
set(method_source "struct Node {\n    var next:*Node;\n    func Next() -> *Node { return next; }\n}\nfunc main() -> int {\n    var node:Node;\n    var node3 = node.Next;\n    return 0;\n}\n")
dmm_test_write( "${source}" "${method_source}")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON code GET "${diagnostics}" errors 0 errorCode)
string(JSON source_line GET "${diagnostics}" errors 0 sourceLine)
string(JSON line GET "${diagnostics}" errors 0 line)
string(JSON column GET "${diagnostics}" errors 0 column)
string(JSON end_column GET "${diagnostics}" errors 0 endColumn)
string(JSON replacement GET "${diagnostics}" errors 0 fix replacement)
string(JSON fix_line GET "${diagnostics}" errors 0 fix line)
string(JSON fix_column GET "${diagnostics}" errors 0 fix column)
if(result EQUAL 0 OR NOT code STREQUAL "S111" OR NOT line EQUAL 8 OR
   NOT column EQUAL 17 OR NOT end_column EQUAL 26 OR
   NOT source_line STREQUAL "    var node3 = node.Next;" OR
   NOT replacement STREQUAL "()" OR NOT fix_line EQUAL 8 OR NOT fix_column EQUAL 26)
    message(FATAL_ERROR "Method reference diagnostic/fix is incorrect: ${diagnostics}")
endif()
execute_process(COMMAND "${COMPILER}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
if(NOT diagnostics MATCHES "invalid.dmm:8:17" OR
   NOT diagnostics MATCHES "var node3 = node.Next;" OR
   NOT diagnostics MATCHES "\\^\\^\\^\\^\\^\\^\\^\\^\\^" OR
   NOT diagnostics MATCHES "help:.*Add '\\(\\)'")
    message(FATAL_ERROR "Human-readable method context is missing: ${diagnostics}")
endif()
string(REPLACE "node.Next;" "node.Next();" corrected "${method_source}")
dmm_test_write( "${source}" "${corrected}")
execute_process(COMMAND "${COMPILER}" "${source}" RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Suggested method correction does not compile: ${diagnostics}")
endif()

# Required arguments, overloads, receiver mismatches, and void methods cannot get an edit.
# The edit belongs after the method name, even inside grouping parentheses.
string(REPLACE "node.Next;" "(node.Next);" grouped "${method_source}")
dmm_test_write( "${source}" "${grouped}")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON fix_column GET "${diagnostics}" errors 0 fix column)
if(result EQUAL 0 OR NOT fix_column EQUAL 27)
    message(FATAL_ERROR "Grouped method fix is at the wrong location: ${diagnostics}")
endif()

string(REPLACE "node.Next;" "Node.Next;" static_source "${method_source}")
string(REPLACE "func Next() -> *Node { return next; }" "static func Next() -> int { return 1; }" static_source "${static_source}")
dmm_test_write( "${source}" "${static_source}")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON replacement GET "${diagnostics}" errors 0 fix replacement)
if(result EQUAL 0 OR NOT replacement STREQUAL "()")
    message(FATAL_ERROR "Valid static receiver did not get a safe fix: ${diagnostics}")
endif()

foreach(declaration IN ITEMS
    "func Next(value:int) -> *Node { return next; }"
    "func Next() -> *Node { return next; } func Next(value:int) -> *Node { return next; }"
    "static func Next() -> int { return 1; }"
    "func Next() -> void {}")
    string(REPLACE "func Next() -> *Node { return next; }" "${declaration}" invalid "${method_source}")
    dmm_test_write( "${source}" "${invalid}")
    execute_process(COMMAND "${COMPILER}" --formatError "${source}"
        RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
    string(JSON code GET "${diagnostics}" errors 0 errorCode)
    string(JSON fix_type TYPE "${diagnostics}" errors 0 fix)
    if(result EQUAL 0 OR NOT code STREQUAL "S111" OR NOT fix_type STREQUAL "NULL")
        message(FATAL_ERROR "Unsafe method fix was offered: ${diagnostics}")
    endif()
endforeach()

# Diagnostics must capture the imported file's line, not the importer's line.
set(imported "${OUTPUT_DIR}/imported/imported.dmm")
dmm_test_write( "${imported}" "package imported;\npub func broken() -> int { return missing; }\n")
dmm_test_write( "${source}" "import \"dmm.test/generated/imported\";\nfunc main() -> int { return 0; }\n")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON filename GET "${diagnostics}" errors 0 filename)
string(JSON source_line GET "${diagnostics}" errors 0 sourceLine)
if(result EQUAL 0 OR NOT filename MATCHES "imported.dmm$" OR
   NOT source_line STREQUAL "pub func broken() -> int { return missing; }")
    message(FATAL_ERROR "Imported source context is incorrect: ${diagnostics}")
endif()

# Source capture strips CRLF and keeps tabs and Unicode; EOF still has a location.
dmm_test_write( "${source}" "func main() -> void {\r\n\tvar text:string = \"ä\"; missing();\r\n}\r\n")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON source_line GET "${diagnostics}" errors 0 sourceLine)
if(result EQUAL 0 OR NOT source_line STREQUAL "\tvar text:string = \"ä\"; missing();")
    message(FATAL_ERROR "CRLF/tab/Unicode source capture failed: ${diagnostics}")
endif()
dmm_test_write( "${source}" "func main() -> void {\n")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON line GET "${diagnostics}" errors 0 line)
string(JSON source_type TYPE "${diagnostics}" errors 0 sourceLine)
if(result EQUAL 0 OR line LESS 1 OR NOT source_type STREQUAL "STRING")
    message(FATAL_ERROR "EOF source location is missing: ${diagnostics}")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(source "${OUTPUT_DIR}/invalid/invalid.dmm")
dmm_test_write( "${source}" "func main() -> void { @; }")
execute_process(COMMAND "${COMPILER}" "${source}" --formatError
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics ENCODING UTF-8)
if(result EQUAL 0)
    message(FATAL_ERROR "Invalid source was accepted")
endif()
if(NOT output STREQUAL "")
    message(FATAL_ERROR "JSON diagnostics wrote unexpected stdout: ${output}")
endif()
string(JSON error_count ERROR_VARIABLE json_error GET "${diagnostics}" summary errorCount)
string(JSON array_count ERROR_VARIABLE array_error LENGTH "${diagnostics}" errors)
string(JSON category ERROR_VARIABLE category_error GET "${diagnostics}" errors 0 category)
string(JSON line ERROR_VARIABLE line_error GET "${diagnostics}" errors 0 line)
string(JSON column ERROR_VARIABLE column_error GET "${diagnostics}" errors 0 column)
string(ASCII 27 escape)
string(FIND "${diagnostics}" "${escape}" ansi_position)
if(json_error OR array_error OR category_error OR line_error OR column_error OR
   error_count LESS 1 OR NOT array_count EQUAL error_count OR
   NOT category STREQUAL "P" OR line LESS 1 OR column LESS 1 OR
   NOT ansi_position EQUAL -1)
    message(FATAL_ERROR "Diagnostics are not valid JSON: ${diagnostics}")
endif()
dmm_test_write( "${source}" "func main() -> void {} $ $ $ $ $ $ $ $ $ $ $ $\n")
execute_process(COMMAND "${COMPILER}" --formatError "${source}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics ENCODING UTF-8)
string(JSON error_count ERROR_VARIABLE json_error GET "${diagnostics}" summary errorCount)
string(JSON array_count ERROR_VARIABLE array_error LENGTH "${diagnostics}" errors)
if(result EQUAL 0 OR NOT output STREQUAL "" OR json_error OR array_error OR
   error_count LESS 10 OR NOT array_count EQUAL error_count)
    message(FATAL_ERROR "Multiple diagnostics were not emitted as one JSON document: ${diagnostics}")
endif()
