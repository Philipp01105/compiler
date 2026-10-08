cmake_minimum_required(VERSION 3.21)
set(values_dir "${OUTPUT_DIR}/values")
file(MAKE_DIRECTORY "${values_dir}")
file(COPY "${SOURCE_DIR}/examples/json_parser/json" DESTINATION "${values_dir}")
file(COPY "${SOURCE_DIR}/tests/stdlib/json_values/json_values.dmm"
          "${SOURCE_DIR}/tests/stdlib/json_values/dmm.manifest" DESTINATION "${values_dir}")

# CLI behavior and generated JSON cases belong to application_examples_contract.
# Keep this contract focused on the owned value API and its lifetime rules.
foreach(level 0 1)
    set(source "${values_dir}/json_values.dmm")
    set(program "${OUTPUT_DIR}/values_${level}.exe")
    execute_process(COMMAND "${COMPILER}" "-O${level}" "${source}" -o "${program}"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 90)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "JSON values/O${level}: ${errors}")
    endif()
    foreach(target elf coff)
        execute_process(COMMAND "${COMPILER}" "-O${level}" --emit=obj "--target=${target}"
            "${source}" -o "${OUTPUT_DIR}/values_${level}_${target}.o"
            RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 90)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "JSON values/${target}/O${level}: ${errors}")
        endif()
    endforeach()
    execute_process(COMMAND "${OUTPUT_DIR}/values_${level}.exe" RESULT_VARIABLE status TIMEOUT 20)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "JSON owned value/access contract O${level}: ${status}")
    endif()
endforeach()

set(prefix "package main; import (\"stdlib/core\" \"stdlib/text\" \"examples/json_parser/json\");\n")
foreach(rejection escape move)
    # Package loading includes sibling .dmm files, so each negative case needs
    # its own module root, separate from the positive fixture and other cases.
    set(work "${OUTPUT_DIR}/rejections/${rejection}")
    file(MAKE_DIRECTORY "${work}")
    file(COPY "${SOURCE_DIR}/examples/json_parser/json"
              "${SOURCE_DIR}/tests/stdlib/json_values/dmm.manifest" DESTINATION "${work}")
    if(rejection STREQUAL "escape")
        set(body "func bad<'a>()->core.Result<json.Value<'a>,json.Error>{var bytes=text.bytes(\"{}\");var owner=json.parse(&bytes)?;return core.Result<json.Value<'a>,json.Error>.Ok(owner.root());} func main()->int{return 0;}")
    else()
        set(body "func main()->int{var bytes=text.bytes(\"{}\");match(json.parse(&bytes)){Err(error)=>return 1;Ok(owner)=>{var value=owner.root();var moved=owner;var kind=value.kind();return 0;}}}")
    endif()
    file(WRITE "${work}/case.dmm" "${prefix}${body}")
    execute_process(COMMAND "${COMPILER}" "${work}/case.dmm" -o "${work}/case.exe"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 90)
    if(status EQUAL 0 OR NOT errors MATCHES "[Bb]orrow|origin")
        message(FATAL_ERROR "JSON document lifetime rejection ${rejection} failed: ${status}: ${errors}")
    endif()
endforeach()
