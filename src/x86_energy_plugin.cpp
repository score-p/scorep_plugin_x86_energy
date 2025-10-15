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

#include <scorep/plugin/util/matcher.hpp>

x86_energy_plugin::x86_energy_plugin(std::map<std::string, std::string> configVars)
: x86_energy_m(
      std::chrono::microseconds(stoi(configVars.at("interval_us"))))
{
    logging::debug() << "Using x86_energy mechanism: " << mechanism.name();

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
            logging::info()
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
x86_energy_plugin::~x86_energy_plugin()
{
    logging::debug() << "plugin sucessfull finalized";
}

void x86_energy_plugin::start()
{
    logging::info() << "Starting x86_energy measurement.";

    x86_energy_m.add_handles(handles);
    x86_energy_thread = std::thread([this]() { this->x86_energy_m.measurement(); });
}

void x86_energy_plugin::stop()
{
    logging::info() << "Stopping x86_energy measurement.";

    x86_energy_m.stop_measurement();
    if (x86_energy_thread.joinable())
    {
        x86_energy_thread.join();
    }
}

void x86_energy_plugin::synchronize(bool is_responsible, SCOREP_MetricSynchronizationMode sync_mode)
{
    logging::info() << "Synching x86_energy measurement.";
}

std::vector<scorep::plugin::metric_property>
x86_energy_plugin::get_metric_properties(const std::string& namePattern)
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

        std::stringstream str;
        str << mechanism.name() << " " << counter;
        auto metric_name = str.str();
        if ( !scorep::plugin::util::matcher(namePattern)(metric_name))
        {
            continue;
        }

        auto metric =
            scorep::plugin::metric_property(metric_name, " Energy Consumption", "J")
                .accumulated_last()
                .value_double()
                .decimal();

        properties.push_back(metric);
    }

    return properties;
}


std::vector<scorep::plugin::measurement_point>
x86_energy_plugin::add_topology_metrics(const SCOREP_MetricTopologyNode* topologyRoot,
                                        const std::string& metricName)
{
    auto granularity_to_domain = [](const auto granularity)
    {
        switch (granularity)
        {
        case x86_energy::Granularity::SYSTEM:
            return SCOREP_METRIC_TOPOLOGY_NODE_DOMAIN_SHARED_MEMORY;
        case x86_energy::Granularity::SOCKET:
            return SCOREP_METRIC_TOPOLOGY_NODE_DOMAIN_SOCKET;
        case x86_energy::Granularity::CORE:
            return SCOREP_METRIC_TOPOLOGY_NODE_DOMAIN_CORE;
        case x86_energy::Granularity::THREAD:
            return SCOREP_METRIC_TOPOLOGY_NODE_DOMAIN_PU;
        }
        return SCOREP_METRIC_TOPOLOGY_NODE_DOMAIN_NONE;
    };

    /* Find counter based on metric name */
    const auto counter = [this](const auto name){
        for (int i = 0; i < static_cast<int>(x86_energy::Counter::SIZE); i++)
        {
            auto counter = static_cast<x86_energy::Counter>(i);

            std::stringstream str;
            str << mechanism.name() << " " << counter;

            /* The metric name must match the mechanism+counter! */
            if (str.str() == name)
            {
                return counter;
            }
        }
        return x86_energy::Counter::SIZE;
    }(metricName);

    auto granularity = mechanism.granularity(counter);
    if (granularity == x86_energy::Granularity::SIZE)
    {
        return {};
    }

    logging::debug() << "Finding responsible topology nodes for counter " << counter << " / " << granularity << " count " << architecture.size(granularity);

    /* collect all nodes for this granularity/domain */
    std::vector<const SCOREP_MetricTopologyNode*> nodes;
    SCOREP_MetricTopology_ForAllResponsiblePerDomain(
        topologyRoot, granularity_to_domain(granularity),
        [](const SCOREP_MetricTopologyNode* node,
           void* cbArg)
    {
        auto* nodes = static_cast<std::vector<const SCOREP_MetricTopologyNode*>*>(cbArg);
        nodes->emplace_back(node);
    }, (void*)&nodes);

    std::vector<scorep::plugin::measurement_point> results;
    for (const auto* node : nodes)
    {
        if (node->id >= architecture.size(granularity))
        {
            continue;
        }

        std::stringstream str;
        str << metricName << "[" << node->id << "]";
        auto unique_metric_name = str.str();

        /* Use first active source which can handle this metric */
        for (auto& active_source : active_sources)
        {
            try
            {
                logging::debug() << "try source: " << active_source->name()
                                 << " for granularity: " << node->id;

                std::vector<x86_energy::SourceCounter> tmp_vec;
                tmp_vec.emplace_back(active_source->get(counter, node->id));
                const auto metric_id = static_cast<int32_t>(handles.size());
                handles.emplace_back(unique_metric_name, unique_metric_name,
                                     std::move(tmp_vec), std::string("E"), false, 0);

                results.emplace_back(metric_id, node);
                break;
            }
            catch (std::runtime_error& e)
            {
                logging::debug() << "Could not access source: " << active_source->name()
                                 << " for granularity: " << node->id << " Reason : " << e.what();
            }
        }
    }

    return results;
}

template <typename C>
void x86_energy_plugin::get_all_values(std::int32_t id, C& cursor)
{

    auto values = x86_energy_m.get_readings(handles[id]);
    for (auto& value : values)
    {
        cursor.write(value);
    }

    logging::debug() << "get_all_values wrote " << values.size() << " values (out of which "
                     << cursor.size() << " are in the valid time range)";
}

std::map<std::string, std::string> x86_energy_plugin::declare_config_vars()
{
    return { { "interval_us", "50000" } };
}

SCOREP_METRIC_PLUGIN_CLASS(x86_energy_plugin, "x86_energy")
