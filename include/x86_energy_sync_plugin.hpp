/*
 * x86_energy_sync_plugin.hpp
 *
 *  Created on: 29.06.2018
 *      Author: gocht
 */

#ifndef INCLUDE_X86_ENERGY_SYNC_PLUGIN_HPP_
#define INCLUDE_X86_ENERGY_SYNC_PLUGIN_HPP_

#include <scorep/plugin/plugin.hpp>
#include <x86_energy.hpp>
#include <x86_energy_metric.hpp>

template <typename T, typename Policies>
using x86_energy_object_id = scorep::plugin::policy::object_id<x86_energy_metric, T, Policies>;

/**
 * Initialization of the plugin.
 *
 * obtaining hostname.
 * get environment
 **/
class x86_energy_sync_plugin
: public scorep::plugin::base<x86_energy_sync_plugin, scorep::plugin::policy::per_thread,
                              scorep::plugin::policy::sync_strict,
                              scorep::plugin::policy::scorep_clock,
                              scorep::plugin::policy::synchronize, x86_energy_object_id>
{
public:
    x86_energy_sync_plugin();
    ~x86_energy_sync_plugin();

    std::vector<scorep::plugin::metric_property> get_metric_properties(const std::string& name);
    void synchronize(bool is_responsible, SCOREP_MetricSynchronizationMode sync_mode);
    void add_metric(x86_energy_metric& m);

    template <typename P>
    void get_current_value(x86_energy_metric& m, P& proxy);

private:
    scorep::plugin::metric_property add_metric_property(const std::string& name, int sensor,
                                                        int node, std::string& quantity);

    bool metric_properties_added = false;

    x86_energy::Mechanism mechanism;
    std::vector<std::unique_ptr<x86_energy::AccessSource>> active_sources;
    x86_energy::Architecture architecture; /**< Architecture tree, e.g. SYSTEM->PKG->...*/

    const std::string prefix_ = "x86_energy/"; /**< TODO reimplement **/
    std::string hostname;
    bool is_resposible = false;
    pid_t responsible_thread = -1;
};

#endif /* INCLUDE_X86_ENERGY_SYNC_PLUGIN_HPP_ */
