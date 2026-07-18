if(NOT GBARECOMP_RUNTIME_CMAKE OR NOT GBARECOMP_TEST_SOURCE OR
   NOT GBARECOMP_TEST_BINARY OR NOT GBARECOMP_TEST_GENERATOR)
    message(FATAL_ERROR "runtime monolith guard test arguments are incomplete")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${GBARECOMP_TEST_SOURCE}"
        -B "${GBARECOMP_TEST_BINARY}"
        -G "${GBARECOMP_TEST_GENERATOR}"
        -DGBARECOMP_RUNTIME_CMAKE=${GBARECOMP_RUNTIME_CMAKE}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)

set(output "${stdout}\n${stderr}")
if(result EQUAL 0)
    message(FATAL_ERROR "stale cartridge monolith configured successfully")
endif()
if(NOT output MATCHES "stale monolithic" OR
   NOT output MATCHES "no longer supported")
    message(FATAL_ERROR
        "configuration failed without the monolith guard diagnostic:\n${output}")
endif()
message(STATUS "runtime monolith guard rejected stale recompiled.cpp")
