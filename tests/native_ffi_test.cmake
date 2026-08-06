cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/dmm.manifest" "module ffi.test/abi\ndmm 2026-09-22-dev\n")
file(COPY "${SOURCE_DIR}/tests/fixtures/native_ffi.dmm" DESTINATION "${OUTPUT_DIR}")
set(drivers "${C_DRIVER}")
if(EXTRA_DRIVER)
    list(APPEND drivers "${EXTRA_DRIVER}")
endif()
foreach(driver IN LISTS drivers)
    get_filename_component(driver_name "${driver}" NAME_WE)
    execute_process(COMMAND "${driver}" -std=c11 -O2 -c "${SOURCE_DIR}/tests/fixtures/native_ffi_reference.c"
        -o "${OUTPUT_DIR}/${driver_name}.o" RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "C ABI reference build failed: ${diagnostics}")
    endif()
    foreach(level 0 1)
        set(image "${OUTPUT_DIR}/${driver_name}_${level}.exe")
        execute_process(COMMAND "${COMPILER}" "-O${level}" --linker-driver "${driver}"
            --native-library "ffi_reference=${OUTPUT_DIR}/${driver_name}.o"
            --dump-native-link "${OUTPUT_DIR}/link_${driver_name}_${level}.txt"
            --dump-ir "${OUTPUT_DIR}/ir_${driver_name}_${level}.txt"
            -o "${image}" "${OUTPUT_DIR}/native_ffi.dmm"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 45)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Native ABI emission/link failed (${driver_name}, O${level}): ${diagnostics}")
        endif()
        execute_process(COMMAND "${image}" RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Native ABI execution failed (${driver_name}, O${level}): checkpoint ${result}: ${diagnostics}")
        endif()
        file(READ "${OUTPUT_DIR}/link_${driver_name}_${level}.txt" link)
        if(NOT link MATCHES "dmm-native-link-v2" OR NOT link MATCHES "runtime-profile=platform" OR
           link MATCHES "never_link_this")
            message(FATAL_ERROR "Incorrect emitted native dependency inventory: ${link}")
        endif()
    endforeach()
endforeach()
