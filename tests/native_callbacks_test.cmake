cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/dmm.manifest" "module ffi.test/callbacks\ndmm 2026-09-22-dev\n")
foreach(source native_callbacks.dmm native_callbacks_linux.dmm native_callbacks_windows.dmm)
    file(COPY "${SOURCE_DIR}/tests/fixtures/${source}" DESTINATION "${OUTPUT_DIR}")
endforeach()
set(drivers "${C_DRIVER}")
if(EXTRA_DRIVER)
    list(APPEND drivers "${EXTRA_DRIVER}")
endif()
foreach(driver IN LISTS drivers)
    get_filename_component(name "${driver}" NAME_WE)
    execute_process(COMMAND "${driver}" -std=c11 -O2 -pthread -c
        "${SOURCE_DIR}/tests/fixtures/native_callbacks_reference.c" -o "${OUTPUT_DIR}/${name}.o"
        RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Callback C reference failed: ${diagnostics}")
    endif()
    foreach(level 0 1)
        execute_process(COMMAND "${COMPILER}" "-O${level}" --linker-driver "${driver}"
            --native-library "callbacks=${OUTPUT_DIR}/${name}.o"
            --dump-ir "${OUTPUT_DIR}/${name}_${level}.ir" -o "${OUTPUT_DIR}/${name}_${level}.exe"
            "${OUTPUT_DIR}/native_callbacks.dmm"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 45)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Callback emission failed: ${diagnostics}")
        endif()
        execute_process(COMMAND "${OUTPUT_DIR}/${name}_${level}.exe"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Callback execution checkpoint ${result}: ${diagnostics}")
        endif()
    endforeach()
    get_filename_component(compiler_directory "${COMPILER}" DIRECTORY)
    if(WIN32)
        set(target coff)
        set(link_options -lkernel32 -lws2_32)
    else()
        set(target elf)
        set(link_options -no-pie -pthread)
    endif()
    foreach(syntax intel att)
        execute_process(COMMAND "${COMPILER}" -O1 -S "--syntax=${syntax}"
            -o "${OUTPUT_DIR}/${name}_${syntax}.s" "${OUTPUT_DIR}/native_callbacks.dmm"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 45)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Callback assembly emission failed: ${diagnostics}")
        endif()
        execute_process(COMMAND "${driver}" "${OUTPUT_DIR}/${name}_${syntax}.s" "${OUTPUT_DIR}/${name}.o"
            # The network bundle includes platform startup and thread support.
            "${compiler_directory}/dmm-runtime/${target}/network-shim.o" ${link_options}
            -o "${OUTPUT_DIR}/${name}_${syntax}.exe"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 30)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Callback assembly linking failed: ${diagnostics}")
        endif()
        execute_process(COMMAND "${OUTPUT_DIR}/${name}_${syntax}.exe"
            RESULT_VARIABLE result ERROR_VARIABLE diagnostics TIMEOUT 20)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Callback assembly checkpoint ${result}: ${diagnostics}")
        endif()
    endforeach()
endforeach()
