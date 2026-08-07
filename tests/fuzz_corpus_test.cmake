cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/dmm.manifest" "module fuzz.test/seeds\ndmm 2026-09-22-dev\n")
set(valid_seeds auto_properties_specializations explicit_lifetime_branch_cleanup
    dynamic_interfaces_nested_sums callable_generic_control_values nested_arrays_borrow_regions
    move_only_enum_control_flow numeric_loops_deferred_control)
foreach(seed IN LISTS valid_seeds)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/fuzz/corpus/ir/${seed}.dmm" "${OUTPUT_DIR}/main.dmm" COPYONLY)
    foreach(level 0 1)
        execute_process(COMMAND "${COMPILER}" "-O${level}" -S --dump-ir "${OUTPUT_DIR}/${seed}_${level}.ir"
            "${OUTPUT_DIR}/main.dmm" -o "${OUTPUT_DIR}/${seed}_${level}.s"
            RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 20)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "IR seed ${seed} at O${level} did not reach lowering: ${errors}")
        endif()
    endforeach()
endforeach()
foreach(seed conditional_properties_no_fallback lifetime_branch_loan_errors generic_interface_conflicts)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/fuzz/corpus/semantic/${seed}.dmm" "${OUTPUT_DIR}/main.dmm" COPYONLY)
    execute_process(COMMAND "${COMPILER}" -S "${OUTPUT_DIR}/main.dmm" -o "${OUTPUT_DIR}/${seed}.s"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 20)
    if(status EQUAL 0 OR NOT errors MATCHES "error\\[")
        message(FATAL_ERROR "Diagnostic seed ${seed} was not rejected normally: ${errors}")
    endif()
endforeach()
