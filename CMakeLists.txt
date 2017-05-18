cmake_minimum_required(VERSION 2.8)
project(x86_energy_plugin)

# Intialize git submodules if not done already
file(GLOB SUBMODULE_FILES "common/*")
list(LENGTH SUBMODULE_FILES COUNT_COMMON)
file(GLOB SUBMODULE_FILES "x86_energy/*")
list(LENGTH SUBMODULE_FILES COUNT_X86_ENERGY)
file(GLOB SUBMODULE_FILES "scorep/*")
list(LENGTH SUBMODULE_FILES COUNT_SCOREP)
if(${COUNT_COMMON} EQUAL 0 OR ${COUNT_SCOREP} EQUAL 0 OR ${COUNT_X86_ENERGY} EQUAL 0 )
    message(STATUS "Initializing git submodule")
    execute_process(COMMAND "git" "submodule" "init" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    execute_process(COMMAND "git" "submodule" "update" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/common;${CMAKE_MODULE_PATH}")

include(common/DefaultBuildType.cmake)

find_package(X86Energy REQUIRED)
include_directories(${X86_ENERGY_INCLUDE_DIRS})
find_package(X86Adapt REQUIRED)
include_directories(${X86_ADAPT_INCLUDE_DIRS})
find_package(Scorep REQUIRED)
include(scorep/ScorepCXXPlugin.cmake)


find_package(MPI)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -Wall -pedantic")

include_directories(include)

# sets the gcc lib path as rpath for build and install directory
string(REGEX REPLACE "bin.*" "lib64" GCC_RPATH ${CMAKE_CXX_COMPILER})
SET(CMAKE_SKIP_BUILD_RPATH  FALSE)
SET(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
SET(CMAKE_INSTALL_RPATH "${GCC_RPATH}")

add_library(x86_energy_sync_plugin SHARED x86_energy_sync_plugin)
add_library(x86_energy_plugin SHARED x86_energy_plugin)

if (MPI_FOUND AND ENABLE_MPI)
    add_definitions(-DHAVE_MPI)
    message(STATUS "Build with MPI support")
    include_directories(${MPI_INCLUDE_PATH})
    set_target_properties(x86_energy_sync_plugin PROPERTIES COMPILE_FLAGS "${MPI_COMPILE_FLAGS}")
    set_target_properties(x86_energy_sync_plugin PROPERTIES LINK_FLAGS "${MPI_LINK_FLAGS}")
    set_target_properties(x86_energy_sync_plugin PROPERTIES LINK_LIBRARIES "${MPI_LIBRARIES}")
elseif(ENABLE_MPI)
    message(FATAL_ERROR "Can't find MPI. Please provide MPI or unset \"ENABLE_MPI\"")
endif()

target_link_libraries(x86_energy_sync_plugin ${X86_ENERGY_LIBRARIES} ${X86_ADAPT_LIBRARIES})
target_link_libraries(x86_energy_plugin ${X86_ENERGY_LIBRARIES} ${X86_ADAPT_LIBRARIES})

link_directories(${HOME}/src)

install(TARGETS x86_energy_sync_plugin LIBRARY DESTINATION lib)
install(TARGETS x86_energy_plugin LIBRARY DESTINATION lib)

