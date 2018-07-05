/*
 * x86_energy_measurement_thread.cpp
 *
 *  Created on: 05.07.2018
 *      Author: gocht
 */

#ifndef INCLUDE_X86_ENERGY_MEASUREMENT_THREAD_CPP_
#define INCLUDE_X86_ENERGY_MEASUREMENT_THREAD_CPP_

#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <vector>

#include <scorep/plugin/plugin.hpp>

#include <x86_energy_metric.hpp>

using scorep::plugin::logging;

class x86_energy_measurement_thread
{
public:
    x86_energy_measurement_thread(std::chrono::microseconds intervall_);
    void add_handles(std::vector<std::reference_wrapper<x86_energy_metric>> handles);
    std::vector<std::pair<scorep::chrono::ticks, double>> get_readings(x86_energy_metric& handle);
    void measurment();
    void stop_measurment();

private:
    /* thread stuff */
    /* minimal time beetween to sensor readings to get a value unequal zero */
    std::chrono::microseconds intervall;
    std::mutex m_mutex;

    bool stop = true;

    std::unordered_map<std::reference_wrapper<x86_energy_metric>,
                       std::vector<std::pair<scorep::chrono::ticks, double>>,
                       std::hash<x86_energy_metric>, std::equal_to<x86_energy_metric>>
        measurments;
};

#endif /* INCLUDE_X86_ENERGY_MEASUREMENT_THREAD_CPP_ */
