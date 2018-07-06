#!/bin/bash

export SCOREP_EXPERIMENT_DIRECTORY="test_trace"
export SCOREP_ENABLE_TRACING="true"
export SCOREP_ENABLE_PROFILING="false"
export SCOREP_METRIC_PLUGINS=x86_energy_plugin
export SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN=x86_energy/BLADE/E
export SCOREP_METRIC_X86_ENERGY_PLUGIN=x86_energy/BLADE/E
export SCOREP_TOTAL_MEMORY=3G
#valgrind --tool=memcheck ./test
#gdb ./test
#LD_DEBUG=all ./test
#lo2s -a -vv -- sleep 5
./test
