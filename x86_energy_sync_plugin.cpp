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

#include <scorep/plugin/plugin.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifdef HAVE_MPI
#include <functional>
#include <mpi.h>
#endif

#include <sys/syscall.h>
#include <sys/types.h>

#include <scorep/plugin/util/matcher.hpp>

extern "C" {
#include <x86_energy.h>
}

bool global_is_resposible_process = false;
pid_t global_responsible_thread = -1;

namespace spp = scorep::plugin::policy;
using scorep_clock = scorep::chrono::measurement_clock;
using scorep::plugin::logging;

/**
 * Our x86_energy metric handle
 **/
class x86_energy_sync_metric
{
public:
    // We need some kind of constructor
    x86_energy_sync_metric(const std::string& full_name_, const std::string& name_, int sensor_,
                           int node_, const std::string& quantity_)
    : mfull_name(full_name_), mname(name_), msensor(sensor_), mnode(node_), mquantity(quantity_)
    {
    }

    // delete copy constructor to avoid ... copies,
    // needs move and default constructor though!
    x86_energy_sync_metric(x86_energy_sync_metric&&) = default;
    x86_energy_sync_metric(const x86_energy_sync_metric&) = delete;
    /* copy-assign */
    x86_energy_sync_metric& operator=(const x86_energy_sync_metric&) = delete;
    /* move assignment */
    x86_energy_sync_metric& operator=(x86_energy_sync_metric&&) = default;

    double first = -1;
    double acct = 0;
    double diff = 0;
    double last = 0;

    /* getter functions */
    std::string name() const
    {
        return mname;
    }

    std::string full_name() const
    {
        return mfull_name;
    }

    int sensor() const
    {
        return msensor;
    }

    int node() const
    {
        return mnode;
    }

    std::string quantity() const
    {
        return mquantity;
    }

    /* functions for measure time handling */
    void set_timepoint()
    {
        last_measurement = current_measurement;
        current_measurement = std::chrono::steady_clock::now();
    }

    auto time_diff()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(current_measurement -
                                                                     last_measurement);
    }

private:
    std::string mfull_name;
    std::string mname;
    /* sensor is the ident number of x86_energy */
    int msensor;
    /* on wich cpu is the sensor */
    int mnode;
    std::string mquantity;

    /* auto is not allowed in non static class member */
    std::chrono::time_point<std::chrono::steady_clock> current_measurement;
    std::chrono::time_point<std::chrono::steady_clock> last_measurement;
};

/**
 * operator to print the metric handle
 *
 **/
std::ostream& operator<<(std::ostream& s, const x86_energy_sync_metric& metric)
{
    s << "(" << metric.full_name() << ", " << metric.sensor() << " on " << metric.node() << ")";
    return s;
}

template <typename T, typename Policies>
using x86_energy_sync_object_id = spp::object_id<x86_energy_sync_metric, T, Policies>;

class x86_energy_sync_plugin
: public scorep::plugin::base<x86_energy_sync_plugin, spp::per_thread, spp::sync_strict,
                              spp::scorep_clock, spp::synchronize, x86_energy_sync_object_id>
{
public:
    /**
     * Initialization of the plugin.
     *
     * obtaining hostname.
     * get environment
     **/
    x86_energy_sync_plugin()
    {

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

        offset = stod(scorep::environment_variable::get("OFFSET", "70000.0"));
        logging::info() << "set offset to " << offset << "mW";

        reading_time = static_cast<std::chrono::milliseconds>(
            stoi(scorep::environment_variable::get("READING_TIME", "0")));
        logging::info() << "set minimum reading time to " << reading_time.count() << "ms";

        /* needed to find sensor_name */
        source = get_available_sources();
        /* test if the the given source is valid */
        if (source == nullptr)
        {
            logging::error() << "x86_energy source is not available -> Throw system_error";
            std::error_code ec(EFAULT, std::system_category());
            throw std::system_error(ec, "x86_energy is not usable");
        }

        logging::info() << "x86_energy_init successful";

        /*
         * this part restores informations saved in the global variables.
         * This is necessary if the plugin gets reinitalisized and the
         * synchronise funciton is not called, for example if PTF is used
         */
        this->responsible_thread = global_responsible_thread;
        this->is_resposible = global_is_resposible_process;
        logging::info() << "got local responsibility from global";

        start_x86_energy();
    }

    /**
     * Destructor
     *
     * Stopping x86_energy
     */
    ~x86_energy_sync_plugin()
    {
        pid_t ptid = syscall(SYS_gettid);
        if (this->is_resposible && (this->responsible_thread == ptid))
        {
            try
            {
                for (int i = 0; i < nr_packages; i++)
                    source->fini_device(i);

                /* pointer is obtained from rapl and can't be freed */
                source = nullptr;
            }
            catch (std::exception& e)
            {
                logging::error() << "Error occoured: " << e.what();
            }

            if (invalid_result_count > 0)
            {
                logging::warn() << "got " << invalid_result_count
                                << " invalid results at host: " << this->hostname;
                logging::warn() << "and " << valid_result_count
                                << " valid results at host: " << this->hostname;
            }
        }
        logging::debug() << "plugin sucessfull finalized";
    }

    /**
     * You don't have to do anything in this method, but it tells the plugin
     * that this metric will indeed be used
     */
    void add_metric(x86_energy_sync_metric& m)
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
    void get_current_value(x86_energy_sync_metric& m, P& proxy)
    {
        // retrun 0 if not responsible. Simulate PER_HOST.
        pid_t ptid = syscall(SYS_gettid);
        if (!this->is_resposible || (this->responsible_thread != ptid))
        {
            proxy.store((int64_t)0);
            return;
        }

        // only possible error is that x86_energy was not initialized before
        if (nr_packages < 0)
        {
            logging::debug() << "start_x86_energy was not called before";
            invalid_result_count++;

            /* no results can be obtained */
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

        double result;
        double energy = 0;
        double energy_offset = 0;

        /* measure time beetween to measurements for one sensor */
        m.set_timepoint();

        /* check if blade is not a substring of the sensor name */
        if (m.name().find("BLADE") == std::string::npos)
        {
            energy = source->get_energy(m.node(), m.sensor());
        }
        else
        {
            /* it solves two problems:
             *  1. the first energy offset can't be really measured because
             *      the time difference is between the construction of
             *      x86_energy_sync_metric and now (std::chrono::time_point
             *      specification)
             *  2. m.first has to be without the offset because only the
             *      RAPL values are important*/
            if (!(m.first < 0))
            {
                /* offset in mW and time_diff is casted to seconds -> energy in mJ,
                 * but J is needed -> convert to SI units (1/1000.0) */
                energy_offset =
                    offset / 1000.0 *
                    std::chrono::duration_cast<std::chrono::duration<double>>(m.time_diff())
                        .count();
            }

            for (int i = 0; i < nr_packages; i++)
                for (int j = 0; j < features; j++)
                {
                    /* only add if the sensor name didn't contain core */
                    if (std::string(source->plattform_features->name[j]).find("core") ==
                        std::string::npos)
                    {
                        energy += source->get_energy(i, source->plattform_features->ident[j]);
                    }
                }
        }

        if (m.first < 0)
        {
            m.first = energy;
        }
        /* the energy will increment continuously,
         * to get more intuitive results the first measurement will be
         * set to zero */
        energy -= m.first;

        /* calc and use energy difference */
        /* adding energy_offset becasue it is only an energy difference to the
         * last measurepoint */
        double energy_diff = energy - m.last + energy_offset;

        /* TODO: Implement Power */
        if (m.time_diff() < reading_time)
        {
            m.acct += energy_diff;
            m.diff += energy_diff;
            result = 0;
        }
        else
        {
            m.acct += energy_diff;
            result = m.acct - m.diff;
        }

        m.last = energy;

        valid_result_count++;

        // energy of x86_energy is in Joule
        proxy.store((int64_t)(result * 1000));
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
    void synchronize(bool is_responsible, SCOREP_MetricSynchronizationMode sync_mode)
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
                start_x86_energy();

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
                start_x86_energy();

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
    std::vector<scorep::plugin::metric_property> get_metric_properties(const std::string& name)
    {
        std::string metric_name = name;
        logging::debug() << "get_event_info called";
        logging::debug() << "metrics name: " << name;
        std::vector<scorep::plugin::metric_property> properties;

        /* Allow the user to prefix the metric with x86_energy/ or not.
         * In the trace it will always be full_name with
         * x86_energy/[COREi, PACKAGEi, GPUi] */
        auto pos_prefix = metric_name.find(prefix_);
        if (pos_prefix == 0)
        {
            metric_name = metric_name.substr(prefix_.length());
        }

        auto pos_quantity = metric_name.find("/");
        std::string quantity;
        if (pos_quantity == std::string::npos)
        {
            logging::warn() << "no physical quantity found using Energy";
            quantity = "E";
        }
        else
        {
            quantity = metric_name.substr(pos_quantity + 1);
            metric_name.erase(pos_quantity);
        }

        /* also match metric name core for core0 and core1 */
        scorep::plugin::util::matcher match(metric_name + "*");

        for (int i = 0; i < source->get_nr_packages(); i++)
        {
            for (int j = 0; j < source->plattform_features->num; j++)
            {
                std::string sensor_name(source->plattform_features->name[j] + std::to_string(i));
                /* converting sensor name to upper case */
                std::transform(sensor_name.begin(), sensor_name.end(), sensor_name.begin(),
                               ::toupper);
                logging::debug() << "found sensor: " << sensor_name;
                if (match(sensor_name))
                {
                    properties.push_back(add_metric_property(
                        sensor_name, source->plattform_features->ident[j], i, quantity));
                }
            }
        }

        /* add blade counter if claimed */
        if (metric_name.find("BLADE") != std::string::npos or
                metric_name.find("blade") != std::string::npos)
        {
            /* rapl has no member blade */
            logging::debug() << "add virtual sensor: BLADE";
            properties.push_back(add_metric_property("BLADE", -1, -1, quantity));
        }

        if (properties.empty())
        {
            logging::fatal() << "No metrics added. Check your metrics!";
        }

        logging::debug() << "get_event_info(" << metric_name << ") Quantity: " << quantity
                         << " returning " << properties.size() << " properties";

        return properties;
    }

private:
    scorep::plugin::metric_property add_metric_property(const std::string& name, int sensor,
                                                        int node, std::string& quantity)
    {
        const std::string full_name = prefix_ + name + std::string("/") + quantity;
        std::string description;

        already_saved_metrics.insert(std::pair<std::string, bool>(full_name, true));

        auto& handle = make_handle(full_name, full_name, name, sensor, node, quantity);
        logging::trace() << "registered handle: " << handle;
        if (quantity == "E")
        {
            /* accumulated_last() like in the hdeem plugin
             * with accumulated_start() you will get very high negative results */
            return scorep::plugin::metric_property(full_name, " Energy Consumption", "J")
                .accumulated_last()
                .value_int()
                .decimal()
                .value_exponent(-3);
        }
        else
        {
            return scorep::plugin::metric_property(full_name, " Unknown quantity", quantity)
                .absolute_point()
                .value_int();
        }
    }

    /** Initalisation of x86_energy. Called after synchronize.
     *
     * if "is_resposible" is true does:
     *
     * setting the number of packages and the number of features
     * init x86_energy
     */
    void start_x86_energy()
    {
        pid_t ptid = syscall(SYS_gettid);
        if (this->is_resposible && (this->responsible_thread == ptid))
        {
            /* init x86_energy at the beginning */
            nr_packages = source->get_nr_packages();
            logging::info() << "Number of nodes is " << nr_packages;
            features = source->plattform_features->num;
            logging::info() << "Number of features is " << features;
            for (int i = 0; i < nr_packages; i++)
                source->init_device(i);
        }
        else
        {
            logging::debug() << "not responsible";
        }
    }

    std::map<std::string, bool> already_saved_metrics;

    const std::string prefix_ = "x86_energy/";
    std::string hostname;
    bool is_resposible = false;
    int nr_packages = -1;
    /* because struct x86_energy_source is from a C library and can't be freed
     * it has to be realeased before the destructor is called*/
    struct x86_energy_source* source;
    int features = 0;

    /* minimal time beetween to sensor readings to get a value unequal zero */
    std::chrono::milliseconds reading_time;
    /* offset for network cardes and so on in the blade */
    double offset;

    pid_t responsible_thread = -1;

    long int invalid_result_count = 0;
    long int valid_result_count = 0;
};

SCOREP_METRIC_PLUGIN_CLASS(x86_energy_sync_plugin, "x86_energy_sync")
