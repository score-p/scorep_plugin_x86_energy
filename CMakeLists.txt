cmake_minimum_required(VERSION 2.8)
project(x86_energy_plugin)

option(ENABLE_MPI "Build with MPI support." ON)

add_subdirectory(extern)

# x86_energy_sync_plugin
add_library(x86_energy_sync_plugin SHARED x86_energy_sync_plugin)
target_compile_features(x86_energy_sync_plugin PUBLIC cxx_std_14)
target_link_libraries(x86_energy_sync_plugin PUBLIC scorep-plugin-cxx x86_energy-static)
target_include_directories(x86_energy_sync_plugin PUBLIC include)


# x86_energy_plugin
add_library(x86_energy_plugin SHARED x86_energy_plugin)
target_compile_features(x86_energy_plugin PUBLIC cxx_std_14)
target_link_libraries(x86_energy_plugin PUBLIC scorep-plugin-cxx x86_energy-static)
target_include_directories(x86_energy_plugin PUBLIC include)

find_package(MPI)

include_directories(include)

if (MPI_FOUND AND ENABLE_MPI)
    add_definitions(-DHAVE_MPI)
    message(STATUS "Build with MPI support")
    target_compile_definitions(x86_energy_sync_plugin PRIVATE MPI_VERSION)
    target_link_libraries(x86_energy_sync_plugin PRIVATE MPI::MPI_C)
elseif (ENABLE_MPI)
    message(FATAL_ERROR "Can't find MPI. Please provide MPI or unset \"ENABLE_MPI\"")
endif()

install(TARGETS x86_energy_sync_plugin
    LIBRARY DESTINATION lib)
install(TARGETS x86_energy_plugin 
    LIBRARY DESTINATION lib)
