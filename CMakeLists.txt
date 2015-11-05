cmake_minimum_required(VERSION 2.8)
project(X86ENERGY_PLUGIN)

option(BACKEND_SCOREP "Build plugin using scorep(ON) or vampirtrace(OFF)" ON)

set(SCOREP_FOUND false)
set(X86_ENERGY_FOUND false)

set(PLUGIN_SOURCE x86_energy_plugin.c)


include(common/FindX86Energy.cmake)
if(X86_ENERGY_FOUND)
    include_directories(${X86E_INC})
else()
    message(SEND_ERROR "X86_Energy was not found but is required!")
endif()


if(BACKEND_SCOREP)
    include(common/FindScorep.cmake)
    if(SCOREP_FOUND)
        include_directories(${SCOREP_INCLUDE_DIRS})
        add_definitions("-DBACKEND_SCOREP")
    else()
        message("Scorep was not found, falling back to VampirTrace!")
    endif()
endif()


if(NOT SCOREP_FOUND OR NOT BACKEND_SCOREP)
    include(common/FindVampirTrace.cmake)
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

include_directories(${X86_ENERGY_INCLUDE_DIRS})
add_library(x86energy_plugin SHARED ${PLUGIN_SOURCE})
target_link_libraries(x86energy_plugin ${X86_ENERGY_LIBRARIES})

install(TARGETS x86energy_plugin LIBRARY DESTINATION lib)
