cmake_minimum_required(VERSION 2.8)
project(x86energy_plugin)

option(BACKEND_SCOREP "Build plugin using scorep(ON) or vampirtrace(OFF)" ON)

set(PLUGIN_SOURCE x86_energy_plugin.c)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/common;${CMAKE_MODULE_PATH}")

find_package(X86Energy REQUIRED)
include_directories(${X86_ENERGY_INCLUDE_DIRS})


if(BACKEND_SCOREP)
    find_package(Scorep)
    if(SCOREP_FOUND)
        include_directories(${SCOREP_INCLUDE_DIRS})
        add_definitions("-DBACKEND_SCOREP")
    else()
        message("Scorep was not found, falling back to VampirTrace!")
    endif()
endif()


if(NOT SCOREP_FOUND OR NOT BACKEND_SCOREP)
    find_package(VampirTrace REQUIRED)
    if(VT_FOUND)
        include_directories(${VT_INCLUDE_DIRS})
        add_definitions("-DBACKEND_VTRACE")
    else()
        message(SEND_ERROR "Vampirtrace was not found but is required!")
    endif()
endif()

#additional c flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE -ggdb")
#debugging c flags
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -Wstrict-prototypes -Wall -Wundef -Wno-long-long -Wsign-compare -Wcomment -pedantic -finline-functions -fno-strict-aliasing")
# release c flags
set(CMAKE_C_FLAGS_RELEASE "-Os")

add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCE})
target_link_libraries(${PROJECT_NAME} ${X86_ENERGY_LIBRARIES} x86_adapt)

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION lib)
