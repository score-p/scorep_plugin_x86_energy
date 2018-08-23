/*
 * Copyright (c) 2016, Technische Universität Dresden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions
 *    and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of
 * conditions and the following disclaimer in the documentation and/or other materials provided with
 * the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <chrono>
#include <iostream>
#include <limits.h>
#include <string>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifdef HAVE_MPI
#include <functional>
#include <mpi.h>
#endif

#include <x86_energy.hpp>
#include <x86_energy_sync_plugin.hpp>

using scorep::plugin::logging;

bool global_is_resposible_process = false;
pid_t global_responsible_thread = -1;

x86_energy_sync_plugin::x86_energy_sync_plugin()
{
    /*
     * this part restores informations saved in the global variables.
     * This is necessary if the plugin gets reinitalisized and the
     * synchronise funciton is not called, for example if PTF is used
     */
    this->responsible_thread = global_responsible_thread;
    this->is_resposible = global_is_resposible_process;

    char c_hostname[HOST_NAME_MAX + 1];
    if (gethostname(c_hostname, HOST_NAME_MAX + 1))
    {
        int errsv = errno;
        switch (errsv)
        {
        case EFAULT:
            throw std::runtime_error("Failed to get local hostname. EFAULT");
            break;
        case EINVAL:
            throw std::runtime_error("Failed to get local hostname. EINVAL");
            break;
        case ENAMETOOLONG:
            throw std::runtime_error("Failed to get local hostname. ENAMETOOLONG");
            break;
        case EPERM:
            throw std::runtime_error("Failed to get local hostname. EPERM");
            break;
        default:
            throw std::runtime_error(std::string("Failed to get local hostname. ERRNO: ") +
                                     std::to_string(errsv));
            break;
        }
    }
    this->hostname = std::string(c_hostname);

    logging::debug("X86_ENERGY_SYNC_PLUGIN") << "Using x86_energy mechanism: " << mechanism.name();

    auto sources = mechanism.available_sources();

    for (auto& source : sources)
    {
        try
        {
            source.init();
            logging::debug() << "Add Source: " << source.name();
            active_sources.push_back(std::make_unique<x86_energy::AccessSource>(std::move(source)));
        }
        catch (std::exception& e)
        {
            logging::debug("X86_ENERGY_SYNC_PLUGIN")
                << "Failed to initialize access source: " << source.name()
                << " error was: " << e.what();
        }
    }

    if (active_sources.empty())
    {
        logging::fatal()
            << "Failed to initialize any available source. x86_energy values won't be available.";
        throw std::runtime_error("Failed to initialize x86_energy access source.");
    }
}

/**
 * Destructor
 *
 * Stopping x86_energy
 */
x86_energy_sync_plugin::~x86_energy_sync_plugin()
{
    logging::debug() << "plugin sucessfull finalized";
}

/**
 * You don't have to do anything in this method, but it tells the plugin
 * that this metric will indeed be used
 */
void x86_energy_sync_plugin::add_metric(x86_energy_metric& m)
{
    logging::info() << "adding x86_energy metric " << m.name();
    logging::debug() << "adding counter for ptid:" << syscall(SYS_gettid);
}

/** Will be called for every event in by the measurement environment.
 * You may or may not give it a value here.
 *
 * @param m contains the sored metric informations
 * @param proxy get and save the results
 *
 * NOTE: In this implemenation we use a few assumptions:
 * * scorep calls at every event all metrics
 * * this metrics are called everytime in the same order
 **/
template <typename P>
void x86_energy_sync_plugin::get_current_value(x86_energy_metric& m, P& proxy)
{
    // retrun 0 if not responsible. Simulate PER_HOST.
    pid_t ptid = syscall(SYS_gettid);
    if (!this->is_resposible || (this->responsible_thread != ptid))
    {
        proxy.store((int64_t)0);
        return;
    }

    if (m.quantity() != "E")
    {
        logging::info() << "Unknown Quantity can't be measured";

        /* no results can be obtained */
        proxy.store((int64_t)0);
        return;
    }

    double energy = m.read();

    // energy of x86_energy is in Joule, retunr in mJ
    proxy.store((int64_t)(energy * 1000));
}

/** function to determine the responsible process for x86_energy
 *
 * If there is no MPI communication, the x86_energy communication is PER_PROCESS,
 * so Score-P cares about everything.
 * If there is MPI communication and the plugin is build with -DHAVE_MPI,
 * we are grouping all MPI_Processes according to their hostname hash.
 * Then we select rank 0 to be the responsible rank for MPI communication.
 *
 * @param is_responsible the Score-P responsibility
 * @param sync_mode sync mode, i.e. SCOREP_METRIC_SYNCHRONIZATION_MODE_BEGIN for non MPI
 *              programs and SCOREP_METRIC_SYNCHRONIZATION_MODE_BEGIN_MPP for MPI program.
 *              Does not deal with SCOREP_METRIC_SYNCHRONIZATION_MODE_END
 */
void x86_energy_sync_plugin::synchronize(bool is_responsible,
                                         SCOREP_MetricSynchronizationMode sync_mode)
{
    logging::debug() << "Synchronize " << is_responsible << ", " << sync_mode;
    if (is_responsible)
    {
        switch (sync_mode)
        {
        case SCOREP_METRIC_SYNCHRONIZATION_MODE_BEGIN:
        {
            logging::debug() << "SCOREP_METRIC_SYNCHRONIZATION_MODE_BEGIN";

            this->is_resposible = true;
            this->responsible_thread = syscall(SYS_gettid);
            logging::debug() << "got responsible ptid:" << this->responsible_thread;

            break;
        }
        case SCOREP_METRIC_SYNCHRONIZATION_MODE_BEGIN_MPP:
        {
            logging::debug() << "SCOREP_METRIC_SYNCHRONIZATION_MODE_BEGIN_MPP";

#ifdef HAVE_MPI
            std::hash<std::string> mpi_color_hash;
            MPI_Comm node_local_comm;
            int myrank;
            int new_myrank;

            // TODO
            // we are assuming, that this is unique enough ....
            // we probably need a rework here
            int hash = abs(mpi_color_hash(this->hostname));

            logging::debug() << "hash value: " << hash;

            MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
            MPI_Comm_split(MPI_COMM_WORLD, hash, myrank, &node_local_comm);
            MPI_Comm_rank(node_local_comm, &new_myrank);

            if (new_myrank == 0)
            {
                this->is_resposible = true;
                logging::debug() << "got responsible process for host: " << this->hostname;
            }
            else
            {
                this->is_resposible = false;
            }

            this->responsible_thread = syscall(SYS_gettid);
            logging::debug() << "got responsible ptid:" << this->responsible_thread;

            MPI_Comm_free(&node_local_comm);
#else
            logging::warn() << "You are using the non MPI version of this "
                               "plugin. This might lead to trouble if there is more "
                               "than one MPI rank per node.";
            this->is_resposible = true;
            this->responsible_thread = syscall(SYS_gettid);
            logging::debug() << "got responsible ptid:" << this->responsible_thread;
#endif
            break;
        }
        case SCOREP_METRIC_SYNCHRONIZATION_MODE_END:
        {
            break;
        }
        case SCOREP_METRIC_SYNCHRONIZATION_MODE_MAX:
        {
            break;
        }
        }
    }

    global_is_resposible_process = this->is_resposible;
    global_responsible_thread = this->responsible_thread;
    logging::debug() << "setting global responsible ptid to:" << this->responsible_thread;
}

/**
 * Convert a named metric (may contain wildcards or so) to a vector of
 * actual metrics (may have a different name)
 *
 * NOTE: Adds the metrics. Currently available metrics are depend on the
 * current system. In RAPL you can found the following possible ones
 *
 *  * package
 *  * core
 *  * gpu
 *  * dram
 *  * dram_ch0
 *  * dram_ch1
 *  * dram_ch2
 *  * dram_ch3
 *
 *  Wildcards are allowed.
 *  The metrics will be changed to upper case letters and the number ot the
 *  package will be added at the end of the name
 */
std::vector<scorep::plugin::metric_property>
x86_energy_sync_plugin::get_metric_properties(const std::string& name)
{
    logging::debug() << "received get_metric_properties(" << name << ")";
    std::vector<scorep::plugin::metric_property> properties;
    if (metric_properties_added)
    {
        logging::debug() << "Tried already to add metric properties. Do nothing.";
        return properties;
    }

    std::vector<x86_energy::SourceCounter> blade_sources;

    for (int i = 0; i < static_cast<int>(x86_energy::Counter::SIZE); i++)
    {
        auto counter = static_cast<x86_energy::Counter>(i);
        auto granularity = mechanism.granularity(counter);

        if (granularity == x86_energy::Granularity::SIZE)
        {
            logging::debug() << "Counter is not available: " << counter << " (Skipping)";

            continue;
        }

        for (auto index = 0; index < architecture.size(granularity); index++)
        {

            std::stringstream str;
            str << mechanism.name() << " " << counter << "[" << index << "]";

            std::string metric_name = str.str();

            std::vector<x86_energy::SourceCounter> tmp_vec;
            for (auto& active_source : active_sources)
            {
                logging::debug() << "try source: " << active_source->name()
                                 << " for granularity: " << index;
                try
                {
                    tmp_vec.emplace_back(active_source->get(counter, index));

                    std::stringstream str;
                    str << active_source->name() << "/" << mechanism.name() << " " << counter << "["
                        << index << "]";
                    std::string metric_name = str.str();

                    auto& handle = make_handle(metric_name, metric_name, metric_name,
                                               std::move(tmp_vec), std::string("E"), false, 0);
                    auto metric =
                        scorep::plugin::metric_property(metric_name, " Energy Consumption", "J")
                            .accumulated_last()
                            .value_int()
                            .decimal()
                            .value_exponent(-3);
                    properties.push_back(metric);

                    if (counter == x86_energy::Counter::PCKG ||
                        counter == x86_energy::Counter::DRAM)
                    {
                        blade_sources.emplace_back(active_source->get(counter, index));
                    }
                    break;
                }
                catch (std::runtime_error& e)
                {
                    logging::debug() << "Could not access source: " << active_source->name()
                                     << " for granularity: " << index << " Reason : " << e.what();
                }
            }
        }
    }

    if (!blade_sources.empty())
    {
        double offset = stod(scorep::environment_variable::get("OFFSET", "70.0"));
        logging::info("X86_ENERGY_SYNC_PLUGIN") << "set offset to " << offset << "W";

        std::string metric_name = "x86_energy/BLADE/E";
        auto& handle = make_handle(metric_name, metric_name, metric_name, std::move(blade_sources),
                                   std::string("E"), true, offset);
        auto metric = scorep::plugin::metric_property(metric_name, " Energy Consumption", "J")
                          .accumulated_last()
                          .value_int()
                          .decimal()
                          .value_exponent(-3);
        properties.push_back(metric);
    }

    if (properties.empty())
    {
        logging::error() << "Did not add any property! There will be no measurments available.";
    }
    metric_properties_added = true;
    return properties;
}

SCOREP_METRIC_PLUGIN_CLASS(x86_energy_sync_plugin, "x86_energy_sync")
