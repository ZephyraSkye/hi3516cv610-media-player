if(EXISTS "${SRC}")
    if(IS_DIRECTORY "${SRC}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${SRC}" "${DST}"
            RESULT_VARIABLE _result
        )
    else()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC}" "${DST}"
            RESULT_VARIABLE _result
        )
    endif()
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Failed to copy ${SRC} -> ${DST}")
    endif()
else()
    message(STATUS "copy_if_exists: ${SRC} not found, skipping")
endif()
