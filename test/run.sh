#!/bin/bash

export SCOREP_EXPERIMENT_DIRECTORY="test_trace"
export SCOREP_ENABLE_TRACING="true"
export SCOREP_METRIC_PLUGINS=x86_energy_sync_plugin
export SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN=x86_energy/BLADE/E
export SCOREP_TOTAL_MEMORY=3G

gdb ./test
