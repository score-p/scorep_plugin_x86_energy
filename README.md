#Score-P power and energy event plugin counter

This is the Score-P power and energy event plugin counter for Intel Sandybridge
and AMD Bulldozer. The plugin supports reading `msr` registers directly or through the `x86_adapt`
library.

##Compilation and Installation

###Prerequisites

To compile this plugin, you need:

* GCC compiler

* `libpthread`

* `libx86_energy` (see [here](https://github.com/tud-zih-energy/x86_energy))

* CMake

* Score-P (or VampirTrace `5.11+`)

* Reading `msr` directly:

    The kernel module `msr` should be active (you might use `modprobe`) and you should have reading
    access to `/dev/cpu/*/msr`.

* Reading energy values through `x86_adapt` (see [here](https://github.com/tud-zih-energy/x86_adapt)):

    The kernel module `x86_adapt_driver` should be active and and should have reading access to
    `/dev/x86_adapt/cpu/*`.

###Build Options

* `X86E_STATIC` (default off)

    Links `x86_energy` lib static.

* `X86E_INC`

    Path to `x86_energy` header files.

* `X86E_LIB`

    Path to `x86_energy` library.

* `X86E_DIR`

    Path to CMake file of `x86_energy` for building purposes. Searches in `X86_ENERGY_DIR` for the
    include files and in `X86_ENERGY_DIR/build` for the library.

* `BACKEND_SCOREP` (default on)

    Build for Score-P (alternative: build for VampirTrace).

###Building

1. Create a build directory

        mkdir build
        cd build

2. Invoking CMake

    Specify the `x86_energy` directory if it is not in the default path with `-DX86E_INC=<PATH>`.
    Default is to link dynamic. To link static turn `-DX86E_STATIC=ON`.

        cmake ..

    Example for a prebuild static linked `x86_energy` which is not in the default path:

        cmake .. -DX86E_INC=$HOME/x86_energy -DX86E_LIB=$HOME/x86_energy/build -DX86E_STATIC=ON

    Example for building `x86_energy` lib and linking it statically:

        cmake .. -DX86E_DIR=$HOME/x86_energy

3. Invoking make

        make

4. Copy libx86energy_plugin.so to a location listed in `LD_LIBRARY_PATH` or add current path to `LD_LIBRARY_PATH` with

        export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:`pwd`

> *Note:*

> If `x86_energy` is linked dynamic then the location of `x86_energy` has to be in the
> `LD_LIBRARY_PATH` as well.

##Usage

###Score-P

To add a kernel event counter to your trace, you have to specify the environment variable
`SCOREP_METRIC_PLUGINS`, e.g.

    export SCOREP_METRIC_PLUGINS="x86energy_plugin"

Then you can select the software events that shall be recorded (see "Available Metrics").

###VampirTrace

To add a kernel event counter to your trace, you have to specify the environment variable
`VT_PLUGIN_CNTR_METRICS`.

###Available Metrics

`SCOREP_METRIC_X86ENERGY_PLUGIN`/`VT_PLUGIN_CNTR_METRICS` specifies the software events that shall
be recorded when tracing an application. You can add the following metrics:

* `*_energy`

    Collect energy consumption information for every avaible counter.

* `*_power`

    Collect power consumption information for every avabile counter.

* `package_energy`

    Collect power consumption information for every package.

* `package_power`

    Collect power consumption information for every package.

* `core_energy`

    Collect power consumption information for every core.

* `core_power`

    Collect power consumption information for every core.

* `gpu_energy`

    Collect power consumption information for every gpu.

* `gpu_power`

    Collect power consumption information for every gpu.

* `dram_energy`

    Collect power consumption information for every dram.

* `dram_power`

    Collect power consumption information for every dram.

E.g.:

    export SCOREP_METRIC_X86ENERGY_PLUGIN="gpu_power"

or for VampirTrace:

    export VT_PLUGIN_CNTR_METRICS=X86ENERGY_*_energy

###Environment variables

* `SCOREP_X86ENERGY_INTERVAL_US`/`VT_X86ENERGY_INTERVAL_US` (default=100000)

    The time in usecs, between two consecutive reads of the power/energy values. A longer interval
    means less disturbance, a shorter interval is more exact.

    On Intel CPUs, the registers are updated roughly every msec. If you choose an interval of 1ms
    you might find highly variating power consumptions. To gain most exact values, you should set
    the amplitude to 10, if you can live with less accuracy, you should set it to 100000.

* `SCOREP_X86ENERGY_CPU_PIN`/`VT_X86ENERGY_CPU_PIN` (default=no pinning)

    The CPU to pin the register reading thread to. The reading thread uses the `msr` files of core
    `<X86_ENERGY_PLUGIN_CPU_PIN>` of each package (default: the first core of a socket). You might
    consider to set it to a free Hyperthread.

* `SCOREP_X86ENERGY_BUF_SIZE`/`VT_X86ENERGY_BUF_SIZE` (default=4M)

    The size of the buffer for storing samples. Can be suffixed with G, M, and K.

    The buffer size is per counter per package, e.g., on a system with 3 energy counters, 2 sockets
    and 4 MB bufer size this would be 24 MB in total.

    Typically, a sample consists of a 8 byte timestamp and 8 byte per selected counter. If the
    buffer is too small, it might not be capable of storing all events. If this is the case, then an
    error message will be printed to `stderr`.

* `SCOREP_X86ENERGY_SYNCHRONOUS`/`VT_X86ENERGY_SYNCHRONOUS` (default=false, *experimental*)

    Save event data synchronously. *Use at your own risk!*

###If anything fails

1. Check whether the plugin library can be loaded from the `LD_LIBRARY_PATH`.

2. Check whether you are allowed to read `/dev/cpu/*/msr`.

3. Write a mail to the author.

##Authors

* Joseph Schuchart (joseph.schuchart at tu-dresden dot de)

* Michael Werner (michael.werner3 at tu-dresden dot de)
