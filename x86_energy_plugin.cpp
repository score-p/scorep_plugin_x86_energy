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
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
 *    and the following disclaimer in the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define ENV_CONFIG_PREFIX "SCOREP_METRIC_X86_ENERGY_PLUGIN_"
#include <scorep/plugin/plugin.hpp>
#include <scorep/plugin/util/matcher.hpp>

#include <ctime>

#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <iostream>
#include <chrono>
#include <system_error>

/* libs for the thread */
#include <thread>
#include <mutex>
#include <condition_variable>

#include <ratio>

extern "C" {
#include <climits>
#include <x86_energy.h>
}


using namespace scorep::plugin::policy;

using scorep::plugin::logging;

// Must be system clock for real epoch!
using local_clock = std::chrono::system_clock;

template <typename T>
static double chrono_to_millis(T duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

template <typename TP>
class scorep_time_converter
{
    using local_duration_t = typename TP::duration;

public:
    scorep_time_converter(TP local_start, TP local_stop, scorep::chrono::ticks scorep_start,
                          scorep::chrono::ticks scorep_stop)
    : local_start_(local_start), scorep_start_(scorep_start)
    {
        const auto local_duration = local_stop - local_start;
        const auto scorep_duration = scorep_stop - scorep_start;
        tick_rate_ = static_cast<double>(scorep_duration.count()) / local_duration.count();
    }

    template <typename T>
    scorep::chrono::ticks to_ticks(const T duration) const
    {
        return to_ticks(std::chrono::duration_cast<local_duration_t>(duration));
    }

    scorep::chrono::ticks to_ticks(const local_duration_t duration) const
    {
        return scorep::chrono::ticks(duration.count() * tick_rate_);
    }

    scorep::chrono::ticks to_ticks(const TP tp) const
    {
        const auto tp_offset = tp - local_start_;
        return scorep_start_ + to_ticks(tp_offset);
    }

private:
    TP local_start_;
    scorep::chrono::ticks scorep_start_;
    double tick_rate_;
};

/**
 * Our x86_energy metric handle
 **/
class x86_energy_metric
{
    public:
        // We need some kind of constructor
        x86_energy_metric(const std::string& full_name_, 
                const std::string& name_, int sensor_, int node_, 
                std::string& quantity_ )
            : mfull_name(full_name_), mname(name_), msensor(sensor_), mnode(node_)
              , mquantity(quantity_)
    {
    }

        // delete copy constructor to avoid ... copies, 
        // needs move and default constructor though!
        x86_energy_metric(x86_energy_metric&&) = default;
        x86_energy_metric(const x86_energy_metric&) = delete;
        x86_energy_metric& operator=(const x86_energy_metric&) = delete;

        /* getter functions */
        const std::string name() const { return mname; }

        const std::string full_name() const { return mfull_name; }

        const int sensor() const { return msensor; }

        const int node() const { return mnode; }

        const std::string quantity() const { return mquantity; }

    private:

        std::string mfull_name;
        std::string mname;
        /* sensor is the ident number of x86_energy */
        int msensor;
        /* on wich cpu is the sensor */
        int mnode;
        std::string mquantity;

        friend std::ostream& operator<<(std::ostream& s, 
                const x86_energy_metric& metric);
};

/**
 * operator to print the metric handle
 * 
 **/
std::ostream& operator<<(std::ostream& s, const x86_energy_metric& metric)
{
    s << "(" << metric.mfull_name << ", " << metric.msensor << " on " 
        << metric.mnode << ")";
    return s;
}

/**
 * like hdeem but for x86_energy and better
 *
 **/

class x86_energy_measurement
{
    public:
        x86_energy_measurement() 
        /* setting default value for reading_time */
            : mreading_time(std::chrono::milliseconds(5)), is_thread(0)
        {
            /* setting correct pointer for x86_energy */
            /* source.reset(get_available_sources_nothread()); */
            source.reset(get_available_sources());
            /* test if the the given source is valid */
            if (source == NULL)
            {
                logging::error() << "x86_energy source is not available -> Throw system_error";
                std::error_code ec (EFAULT, std::system_category());
                throw std::system_error(ec, "x86_energy is not usable");
            }

            /* get system specific numbers */
            mnr_packages = source->get_nr_packages();
            logging::info() << "Number of nodes is " << mnr_packages;
            mfeatures = source->plattform_features->num;
            logging::info() << "Number of features is " << mfeatures;

        }

        ~x86_energy_measurement()
        {
            /* pointer is obtained from rapl and can't be freed */
            source.release();
        }

        const auto& get_readings() const { return readings; }

        const int features() const { return mfeatures; }

        const int nr_packages() const { return mnr_packages; }

        const char * name(int index) const
        {
            if (index < 0 or index >= mfeatures)
                return NULL;
            return source->plattform_features->name[index];
        }

        const int ident(int index) const
        {
            if (index < 0 or index >= mfeatures)
                return -1;
            return source->plattform_features->ident[index];
        }

        void set_reading_time(std::chrono::milliseconds reading_time)
        {
            if (reading_time <= std::chrono::milliseconds(0))
            {
                logging::info() << "Reading time is with " << reading_time.count()
                    << "ms to low. Use default value with " << mreading_time.count()
                    << "ms";
            }
            else
            {
                logging::info()<< "Set minimum reading time to " 
                    << reading_time.count() << "ms";
                mreading_time = reading_time;
            }
        }

        void start()
        {
            logging::debug() << "start thread";
            /* from http://stackoverflow.com/a/32206983 */
            m_stop = false;
            if (is_thread == 0)
            {
                is_thread = 1;
                m_thread = std::thread(&x86_energy_measurement::measurement, this);
            }
        }

        void stop()
        {
            if (m_stop == true)
            {
                logging::debug() << "already stopped, do nothing";
                return;
            }

            logging::debug() << "stop thread";
            /* work also without curly brackets? */
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cond.notify_one();
            m_thread.join();
            logging::debug() << "successfully stopped";
        }

    private:
        /* x86_energy stuff */
        int mnr_packages;
        int mfeatures;
        std::unique_ptr<struct x86_energy_source> source;
        std::map<std::string, std::vector<double>> readings;

        /* thread stuff */
        /* minimal time beetween to sensor readings to get a value unequal zero */
        std::chrono::milliseconds mreading_time;
        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cond;
        bool m_stop;
        int is_thread;


        void measurement()
        {
            /* init x86_energy at the beginning */
            for(int i = 0; i < mnr_packages; i++)
                source->init_device(i);

            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_stop)
            {
                read();
                m_cond.wait_for(lock, mreading_time);
                /* std::this_thread::sleep_for(mreading_time); */
            }

            /* fini x86_energy at the end */
            for(int i = 0; i < mnr_packages; i++)
                source->fini_device(i);
        }
        
        void read()
        {
            /* read and save the sensor values */
            for(int i = 0; i < mnr_packages; i++)
            {
                for(int j = 0; j < mfeatures; j++)
                {
                    /* save sensor results in map with key sensor_name + 
                     * package_number + quantity
                     * sensor_name is used from x86_energy 
                     * save energy and power, select later, simpler */
                    /* energy */
                    readings[name(j) + std::to_string(i) + "E"].push_back(
                            source->get_energy(i, ident(j)));
                    /* power */
                    readings[name(j) + std::to_string(i) + "P"].push_back(
                            source->get_power(i, ident(j)));
                }
            }
        }

};

template <typename P, typename Policies>
using x86_energy_object_id = object_id<x86_energy_metric, P, Policies>;

class x86_energy_plugin
    : public scorep::plugin::base<x86_energy_plugin, async, per_host, post_mortem, scorep_clock, x86_energy_object_id>
{
public:

    x86_energy_plugin()
    {
        /* get environment */
        offset = stod(scorep::environment_variable::get("OFFSET", "70000.0"));
        logging::info()<< "set offset to " << offset << "mW";

        std::string env_var("INTERVAL_US");
        int def_value = 5;
        /* try old environment first */
        if (scorep::environment_variable::get(env_var) != "")
        {
            logging::warn() << "using old Environment variable " << env_var
                            << "please use READING_TIME instead";
        }
        /* otherwise use current environment variable */
        else
        {
            env_var = "READING_TIME";
            def_value = 0;
        }
        auto reading_time = static_cast<std::chrono::milliseconds>
            (stoi(scorep::environment_variable::get(env_var, std::to_string(def_value))));

        x86_energy.set_reading_time(reading_time);
    }

public:
    void add_metric(x86_energy_metric& handle)
    {
        // We actually don't need to do a thing! :-)
    }

    void start()
    {
        assert(!stopped);
        if (started)
        {
            return;
        }

        local_start = local_clock::now();
        scorep_start = scorep::chrono::measurement_clock::now();
        x86_energy.start();

        started = true;
        logging::info() << "Successfully started x86_energy measurement.";
    }

    void stop()
    {
        if (!started)
        {
            logging::warn() << "Trying to stop without being started.";
            return;
        }
        if (stopped)
        {
            return;
        }

        local_stop = local_clock::now();
        scorep_stop = scorep::chrono::measurement_clock::now();
        stopped = true;

        try
        {
            x86_energy.stop();
        }
        catch (std::exception& ex)
        {
            logging::error() << "X86_Energy could not stop: " <<  ex.what();
        }

        auto tp_after_stop = local_clock::now();
        readings = x86_energy.get_readings();
        /* all vectors have the same length */
        if (readings.empty() == true)
            readings_size = 0;
        else
            readings_size = readings.begin()->second.size();

        logging::info() << "Successfully stopped x86_energy measurement and retrieved "
                        << readings_size << " values"
                        << " in " << chrono_to_millis(local_clock::now() - tp_after_stop) << " ms.";
        logging::debug() << "scorep(ticks) start: " << scorep_start.count() << ", stop: " << scorep_stop.count();
        logging::debug() << "local(sys,us) start: " << local_start.time_since_epoch().count()
                         << ", stop: " << local_stop.time_since_epoch().count();
    }

    void synchronize(bool is_responsible, SCOREP_MetricSynchronizationMode sync_mode)
    {
        logging::debug() << "Synchronize " << is_responsible << ", " << sync_mode;
    }

    template <typename C>
    void get_all_values(x86_energy_metric& handle, C& cursor)
    {
        /* stop(); // Should be already called, but just to be sure. */
        auto tp_start = local_clock::now();
        if (!stopped)
        {
            throw std::runtime_error("Could not stop x86_energy measurement to get values.");
        }
        if (readings.empty())
        {
            logging::error() << "No x86_energy readings available.";
            return;
        }
        
        /* converting sensor name to lower case */
        auto sensor_name = handle.name();
        std::transform(sensor_name.begin(), sensor_name.end(), 
                sensor_name.begin(), ::tolower);
        /* only for real sensors exist the map entry
         * but also key values that didn't exist are accessible
         * use lower case keys because of the performance in the measure
         * thread*/
        auto sensor_data = readings[sensor_name + handle.quantity()];

        auto converter =
            scorep_time_converter<local_clock::time_point>(local_start, local_stop, scorep_start, scorep_stop);
        local_clock::duration duration_actual;
        scorep::chrono::ticks scorep_start_actual;

        duration_actual = local_stop - local_start;
        scorep_start_actual = scorep_start;


        // We must use double here to avoid too much precision loss e.g. from integers in microseconds
        const double sampling_period = static_cast<double>(duration_actual.count()) / readings_size;

        logging::trace() << "Using effective sampling period of " << sampling_period << " sysclock ticks (usually us)";

        const auto index_to_scorep_ticks = [sampling_period, scorep_start_actual, converter](size_t index)
        {
            // TODO Add offset, needs further time computation
            return scorep_start_actual +
                   converter.to_ticks(local_clock::duration(static_cast<int64_t>(sampling_period * index)));
        };

        logging::trace() << "Reading " << readings_size << " values from sensor " << handle.name();
        cursor.resize(readings_size);

        double value = 0;
        const double sampling_period_secs =  std::chrono::duration_cast
            <std::chrono::duration<double>>(duration_actual).count() / readings_size;
        /* start calculation and filling of the cursor */
        for (int i = 0; i < readings_size; i++)
        {
            if (handle.name() != "BLADE")
            {
                /* scale the results with the first measured value */
                value = sensor_data[i] - sensor_data[0];
            }
            else
            {
                if (handle.quantity() == "E")
                {
                    /* for energy the offset power has to be multiplied with 
                     * the time 
                     * the time is casted to seconds*/
                    value = offset/1000 * sampling_period_secs * i;
                }
                else if (handle.quantity() == "P")
                {
                    /* and for power the offset has to be simply added */
                    value = offset/1000;
                }

                for (auto point : readings)
                {
                    /* only add if the sensor name didn't contain core
                     * or one number is negative (for blade) 
                     * and if the quantity of the blade matches*/
                    if (point.first.find("core") == std::string::npos and 
                            point.first.find("blade") == std::string::npos and
                            // use pointer because of operator== from
                            // string
                            &point.first.back() == handle.quantity())
                    {
                        value += (point.second[i] - point.second[0]);
                    }
                }
            }

            /* 1000 because the values from rapl are in W or J and has to
             * be in mW or mJ */
            cursor.store(index_to_scorep_ticks(i), (int64_t)(value * 1000));
        }
        if (cursor.size() == 0)
        {
            logging::warn() << "no valid measurements are in the time range. Total measurements: "
                            << readings_size;
        }
        logging::debug() << "get_all_values wrote " << readings_size << " values (out of which " << cursor.size()
                         << " are in the valid time range) in "
                         << chrono_to_millis(local_clock::now() - tp_start) << " ms.";
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
        get_metric_properties(const std::string& name)
        {
            std::string metric_name(name);
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

            /* compatible with old plugin interface */
            auto pos_quantity = metric_name.find("/");
            auto pos_quantity_old = metric_name.find_last_of("_");
            std::string quantity;
            if (metric_name.substr(pos_quantity_old + 1) == "power")
            {
                logging::warn() << "use old interface for quantity";
                quantity="P";
                metric_name.erase(pos_quantity_old);
            }
            else if (metric_name.substr(pos_quantity_old + 1) == "energy")
            {
                logging::warn() << "use old interface for quantity";
                quantity="E";
                metric_name.erase(pos_quantity_old);
            }
            else if (pos_quantity == std::string::npos)
            {
                logging::warn() << "no physical quantity found using Energy";
                quantity="E";
            }
            else
            {
                quantity=metric_name.substr(pos_quantity+1);
                metric_name.erase(pos_quantity);
            }

            /* also match metric name core for core0 and core1 */
            scorep::plugin::util::matcher match(metric_name + "*");
            for (int i = 0; i < x86_energy.nr_packages(); i ++)
            {
                for (int j = 0; j < x86_energy.features(); j++)
                {
                    std::string sensor_name(x86_energy.name(j)
                            + std::to_string(i));
                    /* converting sensor name to upper case */
                    std::transform(sensor_name.begin(), sensor_name.end(), 
                            sensor_name.begin(), ::toupper);
                    logging::debug() << "found sensor: " << sensor_name;
                    logging::debug() << match(sensor_name);;

                    if (match(sensor_name))
                    {
                        properties.push_back(add_metric_property(sensor_name, 
                                    x86_energy.ident(j), i, quantity));
                    }
                }
            }

            if (properties.empty())
            {
                logging::fatal()<< "No metrics added. Check your metrics!";
            }
            /* add blade counter if necessary */
            else if (match("BLADE") or match("blade"))
            {
                /* rapl has no member blade */
                logging::debug() << "add virtual sensor: BLADE";
                properties.push_back(add_metric_property("BLADE", -1, -1, quantity));
            }

            logging::debug() << "get_event_info(" << metric_name << ") Quantity: "
                << quantity <<" returning " << properties.size() << " properties";

            return properties;
        }

private:
    const std::string prefix_ = "x86_energy/";
    scorep::plugin::metric_property add_metric_property(const std::string& name, 
            int sensor, int node, std::string quantity)
    {
        const std::string full_name = prefix_ + name + std::string("/") + quantity;
        std::string description;

        auto& handle = make_handle(full_name, full_name, name, sensor, node, 
                quantity);
        logging::trace() << "registered handle: " << handle;
        if (quantity == "E")
        {
            /* accumulated_last() like in the hdeem plugin 
             * with accumulated_start() you will get very high negative results */
            return scorep::plugin::metric_property(full_name, 
                    " Energy Consumption" , "mJ").accumulated_last().value_int();
        }
        else if (quantity == "P")
        {
            return scorep::plugin::metric_property(full_name, 
                    " Power Consumption" , "mW").absolute_last().value_int();
        }
        else
        {
            return scorep::plugin::metric_property(full_name, 
                    " Unknown quantity" , quantity).absolute_point().value_int();
        }

    }

    scorep::chrono::ticks scorep_start, scorep_stop;
    std::chrono::time_point<local_clock> local_start, local_stop;

    x86_energy_measurement x86_energy;
    bool started = false;
    bool stopped = false;
    std::map<std::string, std::vector<double>> readings;
    int readings_size;

    /* offset for network cardes and so on in the blade */
    double offset;

};

SCOREP_METRIC_PLUGIN_CLASS(x86_energy_plugin, "x86_energy")
