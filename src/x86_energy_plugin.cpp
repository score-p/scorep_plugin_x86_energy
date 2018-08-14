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

#include <climits>
#include <ctime>

#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

/* libs for the thread */
#include <condition_variable>
#include <mutex>
#include <thread>

#include <ratio>

#include <x86_energy_plugin.hpp>

x86_energy_plugin::x86_energy_plugin()
: x86_energy_m(
      std::chrono::microseconds(stoi(scorep::environment_variable::get("interval_us", "50000"))))
{
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
            logging::info("X86_ENERGY_SYNC_PLUGIN")
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

void x86_energy_plugin::add_metric(x86_energy_metric& handle)
{
    // We actually don't need to do a thing! :-)
}

void x86_energy_plugin::start()
{

    x86_energy_thread = std::thread([this]() { this->x86_energy_m.measurment(); });

    logging::info() << "Successfully started x86_energy measurement.";
}

void x86_energy_plugin::stop()
{
    x86_energy_m.stop_measurment();
    if (x86_energy_thread.joinable())
    {
        x86_energy_thread.join();
    }
}

void x86_energy_plugin::synchronize(bool is_responsible, SCOREP_MetricSynchronizationMode sync_mode)
{
}

std::vector<scorep::plugin::metric_property>
x86_energy_plugin::get_metric_properties(const std::string& name)
{
    std::vector<scorep::plugin::metric_property> properties;
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
                    auto& handle = make_handle(metric_name, metric_name, metric_name,
                                               std::move(tmp_vec), std::string("E"), false, 0);

                    auto metric =
                        scorep::plugin::metric_property(metric_name, " Energy Consumption", "J")
                            .accumulated_last()
                            .value_double()
                            .decimal();

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
                          .value_double()
                          .decimal();
        properties.push_back(metric);
    }

    if (properties.empty())
    {
        logging::fatal() << "Did not add any property! There will be no measurments available.";
    }
    x86_energy_m.add_handles(get_handles());

    return properties;
}

template <typename C>
void x86_energy_plugin::get_all_values(x86_energy_metric& handle, C& cursor)
{

    auto values = x86_energy_m.get_readings(handle);
    for (auto& value : values)
    {
        cursor.write(value);
    }

    logging::debug() << "get_all_values wrote " << values.size() << " values (out of which "
                     << cursor.size() << " are in the valid time range)";
}

SCOREP_METRIC_PLUGIN_CLASS(x86_energy_plugin, "x86_energy")
