# Score-P power and energy event plugin counter

This is the Score-P power and energy event plugin counter for Intel Sandy Bridge and 
AMD Bulldozer. The plugin supports reading `msr` registers directly or through the 
`x86_adapt` library.
For the original synchronous C version with Vampir Trace support please look
at Commit [a11bbb1](https://github.com/score-p/scorep_plugin_x86_energy/commit/a11bbb1).
For a C version with an interface like the HDEEM plugin and Vampir Trace support at
Commit [0238bba](https://github.com/score-p/scorep_plugin_x86_energy/commit/0238bba). For the documentation of this
plugin version you should consider this ReadMe instead.

## Compilation and Installation

### Prerequisites

To compile this plugin, you need:

* A C++ 14 compiler

* `libx86_energy` (see [here](https://github.com/tud-zih-energy/x86_energy))

* CMake

* Score-P Version 2+ (`SCOREP_METRIC_PLUGIN_VERSION` >= 1 (for C++ version)

* For the use of x86_energy:

   Reading `msr` directly:

      The kernel module `msr` should be active (you might use `modprobe`) and you should have reading
      access to `/dev/cpu/*/msr`.

   Reading energy values through `x86_adapt` (see [here](https://github.com/tud-zih-energy/x86_adapt)):

      The kernel module `x86_adapt_driver` should be active and and should have reading access to
      `/dev/x86_adapt/cpu/*`.

### Build Options

* `X86Energy_STATIC` (default on)

    Links `x86_energy` lib static.

* `X86_ENERGY_INCLUDE_DIRS` (usually will be found automatically)

    Path to `x86_energy` header files.

* `X86_ENERGY_LIBRARIES` (usually will be found automatically)

    Should point to the `x86_energy` library (shared or static), including
    the path

* `SCOREP_CONFIG`

    Path to the `scorep-config` tool including the file name

    > Note: If you have `scorep-config` in your `PATH`, it should be found by CMake.

* `CMAKE_INSTALL_PREFIX`

    Directory where the resulting plugin will be installed (`lib/` suffix will be added)

* `ENABLE_MPI`

    Enables MPI communication for the sync plugin, and allows to run more than one MPI process
    per node.

### Building

1. Create a build directory

        mkdir build
        cd build

2. Invoking CMake

    Specify the `x86_energy` directory if it is not in the default path with `-DX86_ENERGY_INCLUDE_DIRS=<PATH>`.
    Default is to link static. To link static turn `-DX86Energy_STATIC=OFF`.

        cmake ..

     > Note: If x86_energy can't be found it will be build automatically with 
     > static linked msr. But before this plugin is build x86_energy will be 
     > installed so set an CMAKE_INSTALL_PREFIX like 

     >       cmake -DCMAKE_INSTALL_PREFIX=$PWD/local ..

     > or have write permissions to the default INSTALL_PREFIX, mostly /usr/local

    Example for a prebuild static linked `x86_energy` which is not in the default path:

        cmake .. -DX86_ENERGY_INCLUDE_DIRS=$HOME/x86_energy -DX86_ENERGY_LIBRARIES=$HOME/x86_energy/build -DX86Energy_STATIC=ON

    If x86_energy is automatically build because it isn't in the path the
    same Build Options can be used like for a native build of x86_energy (see [here](https://github.com/tud-zih-energy/x86_adapt)).

    Example for build x86_energy with x86_adapt support and x86_adapt in
    the LD_LIBRARY_PATH:

         cmake -DCMAKE_INSTALL_PREFIX=$PWD/local -DX86_ADAPT=On ..

3. Invoking make

        make

4. Install the files (optional)

        make install

> Note: Make sure to add the subfolder `lib` to your `LD_LIBRARY_PATH` or if you
> didn't use make install that the plugin is in a location listed in 
> `LD_LIBRARY_PATH`. For example add the build directory to `LD_LIBRARY_PATH`:

>       export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$PWD

> If `x86_energy` is linked dynamic then the location of `x86_energy` has to be in the
> `LD_LIBRARY_PATH` as well.

## Usage

### Score-P

To add a x86_energy metric to your trace, you have to add `x86_energy_plugin` to the environment variable
`SCOREP_METRIC_PLUGINS`. This is the preferred version for tracing. It preserves the best possible
time resolution of the measurements.

To add the synchronous metric, add `x86_energy_sync_plugin` to the environment variable
`SCOREP_METRIC_PLUGINS`. This can be useful for profiling. Measurements are only taken at
events, e.g. enter / leave of regions.

You have to add the list of the metric channel you are interested in to the environment
variable `SCOREP_METRIC_X86_ENERGY_PLUGIN` or `SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN`.

Note: Score-P does not support per-host post-mortem plugins with profiling. If you want to
use the post-mortem (`x86_energy_plugin`) plugin, you should enable tracing and disable profiling by using:

      export SCOREP_ENABLE_PROFILING="false"
      export SCOREP_ENABLE_TRACING="true"

The sync plugin (`x86_energy_sync_plugin`) works with profiling and tracing.

> Note: The plugin is implemented as a strictly synchronous plugin. Therefore, it measures per
> thread. As x86_energy can just be used nodewide, the plugin does some thread and if enabled MPI
> operations in order to obtain the responsible thread and process. The trace file might hold some
> traces that are reported as 0. The profile might report wrong profiling statistics. Moreover, the
> plugin reports mJ as interfer values. This operations are necessary to be compatible with PTF
> (http://periscope.in.tum.de/)
 
> The interface for this plugin is built like the correspondending hdeem plugin.

### Available Metrics

`SCOREP_METRIC_X86_ENERGY_PLUGIN` or 
`SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN` specifies the software events that shall
be recorded when tracing an application. You can add the following metrics:

* `*/E`
    or old syntax: `*_energy`

    Collect energy consumption information for every available counter.

* `*/P`
    or old syntax: `*_power`

    Collect power consumption information for every available counter.

* `PACKAGE/E`
    or old syntax: `package_energy`

    Collect power consumption information for every package.

* `PACKAGE/P`
    or old syntax: `package_power`

    Collect power consumption information for every package.

* `CORE/E`
    or old syntax: `core_energy`

    Collect power consumption information for every core.

* `CORE/P`
    or old syntax: `core_power`

    Collect power consumption information for every core.

* `GPU/E`
    or old syntax: `gpu_energy`

    Collect power consumption information for every gpu.

* `GPU/P`
    or old syntax: `gpu_power`

    Collect power consumption information for every gpu.

* `DRAM/E`
    or old syntax: `dram_energy`

    Collect power consumption information for every dram.

* `DRAM/P`
   or old syntax: `dram_power`

    Collect power consumption information for every dram.

* `PACKAGE0/E`, etc.

    Collect power consumption information for package on node 0.
    This also works with every other metric.

E.g.:

    export SCOREP_METRIC_X86_ENERGY_PLUGIN="GPU/P" or
    export SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN="GPU/P"

### Environment variables

* `SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN`/`SCOREP_METRIC_X86_ENERGY_PLUGIN`

    Comma-separated list of sensors, and physical quantity. Can also contain wildcards, e.g.
    `BLADE/E,*/E`

* `SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN_VERBOSE`/`SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN_VERBOSE` (default `WARN`)

    Controls the output verbosity of the plugin. Possible values are: `FATAL`, `ERROR`, `WARN`,
    `INFO`, `DEBUG`. If set to any other value, `DEBUG` is used. Case in-sensitive.

* `SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN_OFFSET` (default `70000.0`)

    Can be set to any double variable. Defines the offset in mW that is added to 
    the blade, because with RAPL only the energy consumption of the
    CPU is measurable. Blade is the sum of all RAPL values excepted the core
    because there are included in package.
    The default value of 70000.0 mW was measured on Taurus

* `SCOREP_X86_ENERGY_PLUGIN_READING_TIME` (default `5`)
   or like the old plugin: `SCOREP_X86_ENERGY_PLUGIN_INTERVALL_US`

    The time in millisecs, between two consecutive reads of the power/energy values. A longer interval
    means less disturbance, a shorter interval is more exact.

    On Intel CPUs, the registers are updated roughly every msec. If you choose an interval of 1ms
    you might find highly variating power consumptions. To gain most exact values, you should set
    the amplitude to 1, if you can live with less accuracy, you should set it to 100.

* `SCOREP_METRIC_X86_ENERGY_SYNC_PLUGIN_READING_TIME` (default `0`)

    Can be set to any int variable. Defines the minimal time between to sensor 
    readings in micro seconds that will return an number unequal to zero.
    For compatibility with Periscope this should be set to zero.

### If anything fails

1. Check whether the plugin library can be loaded from the `LD_LIBRARY_PATH`.

2. Check whether you are allowed to read `/dev/cpu/*/msr` and your x86_energy 
   functionality is working properly.
   Consulting the x86_energy repo for some suggestions.

3. Write a mail to the author.

## Authors

### Original C version
* Joseph Schuchart (joseph.schuchart at tu-dresden dot de)
* Michael Werner (michael.werner3 at tu-dresden dot de)

### C++ versions
* Andreas Gocht  <andreas.gocht  at tu-dresden dot de>
* Sven Schiffner (sven.schiffner at tu-dresden dot de)

