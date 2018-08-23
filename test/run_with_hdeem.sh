#!/bin/bash

rm -r test_trace*

export SCOREP_EXPERIMENT_DIRECTORY="test_trace"
export SCOREP_ENABLE_TRACING="true"
export SCOREP_ENABLE_PROFILING="false"
export SCOREP_METRIC_PLUGINS=x86_energy_sync_plugin
export SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN=x86_energy/BLADE/E,CPUEnergy
#export SCOREP_METRIC_X86_ENERGY_PLUGIN=x86_energy/BLADE/E
export SCOREP_TOTAL_MEMORY=3G
export SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN_VERBOSE=DEBUG
export SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN_OFFSET=67.0
export X86_ENERGY_SOURCE="x86a-rapl"
#valgrind --tool=memcheck ./test
#gdb ./test
#LD_DEBUG=all ./test
#lo2s -a -vv -- sleep 5

startHdeem
./test
stopHdeem
checkHdeem
