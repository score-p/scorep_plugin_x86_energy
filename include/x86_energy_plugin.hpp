/*
 * x86_energy_plugin.hpp
 *
 *  Created on: 05.07.2018
 *      Author: gocht
 */

#ifndef INCLUDE_X86_ENERGY_PLUGIN_HPP_
#define INCLUDE_X86_ENERGY_PLUGIN_HPP_

#include <scorep/plugin/plugin.hpp>
#include <x86_energy_measurement_thread.hpp>
#include <x86_energy_metric.hpp>

using namespace scorep::plugin::policy;

using scorep::plugin::logging;

template <typename P, typename Policies>
using x86_energy_object_id = object_id<x86_energy_metric, P, Policies>;

class x86_energy_plugin
    : public scorep::plugin::base<x86_energy_plugin, async, per_host, post_mortem, scorep_clock,
                                  x86_energy_object_id>
{
public:
    x86_energy_plugin();
    void add_metric(x86_energy_metric& handle);
    void start();
    void stop();
    void synchronize(bool is_responsible, SCOREP_MetricSynchronizationMode sync_mode);

    template <typename C>
    void get_all_values(x86_energy_metric& handle, C& cursor);

    std::vector<scorep::plugin::metric_property> get_metric_properties(const std::string& name);

private:
    scorep::plugin::metric_property add_metric_property(const std::string& name, int sensor,
                                                        int node, const std::string& quantity);

    x86_energy::Mechanism mechanism;
    std::vector<std::unique_ptr<x86_energy::AccessSource>> active_sources;
    x86_energy::Architecture architecture; /**< Architecture tree, e.g. SYSTEM->PKG->...*/
    x86_energy_measurement_thread x86_energy_m;
    std::thread x86_energy_thread;

    const std::string prefix_ = "x86_energy/"; /**<TODO reimplement*/
};

#endif /* INCLUDE_X86_ENERGY_PLUGIN_HPP_ */
