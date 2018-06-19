cmake_minimum_required (VERSION 3.8)

function(git_clone_checkout target_dir)
    # check if we need to download the anything or just update 
    file(GLOB SUBMODULE_FILES "${target_dir}/*")
    list(LENGTH SUBMODULE_FILES COUNT_FILES)
    if (${COUNT_FILES} EQUAL 0)
        message(STATUS "Initializing git submodule for ${target_dir}")
        execute_process(
            COMMAND 
                "git" "submodule" "init" "--" "${target_dir}"
            WORKING_DIRECTORY
                "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
            OUTPUT_QUIET
            )
        if (NOT ${result} EQUAL 0)
            message(SEND_ERROR "${error}")
        endif()
        execute_process(
            COMMAND
                "git" "submodule" "update" "--" "${target_dir}"
            WORKING_DIRECTORY
                "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
            OUTPUT_QUIET
            )
        if (NOT ${result} EQUAL 0)
            message(SEND_ERROR "${error}")
        endif()
    endif()
endfunction(git_clone_checkout)

git_clone_checkout("${CMAKE_CURRENT_LIST_DIR}/scorep_plugin_cxx_wrapper")
add_subdirectory(scorep_plugin_cxx_wrapper)

git_clone_checkout("${CMAKE_CURRENT_LIST_DIR}/x86_energy")
set(X86_ENERGY_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(x86_energy)

