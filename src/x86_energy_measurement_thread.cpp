/*
 * x86_energy_measurement_thread.cpp
 *
 *  Created on: 05.07.2018
 *      Author: gocht
 */

#include <x86_energy_measurement_thread.hpp>

x86_energy_measurement_thread::x86_energy_measurement_thread(std::chrono::microseconds intervall_)
: intervall(intervall_)
{
}

void x86_energy_measurement_thread::add_handles(const std::vector<x86_energy_metric>& handles)
{
    for (auto& handle : handles)
    {
        measurments.insert(std::make_pair(std::ref(const_cast<x86_energy_metric&>(handle)),
                                          std::vector<std::pair<scorep::chrono::ticks, double>>()));
    }
}

void x86_energy_measurement_thread::measurment()
{
    stop = false;

    while (!stop)
    {
        try
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& metric_it : measurments)
            {
                auto value = metric_it.first.get().read();
                auto tick = scorep::chrono::measurement_clock::now();
                metric_it.second.push_back(std::make_pair(tick, value));
            }
        }
        catch (scorep::exception::null_pointer& e)
        {
            logging::warn() << "Score-P Clock not set.";
        }
        std::this_thread::sleep_for(intervall);
    }
}

std::vector<std::pair<scorep::chrono::ticks, double>>
x86_energy_measurement_thread::get_readings(x86_energy_metric& handle)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return measurments[handle];
}

void x86_energy_measurement_thread::stop_measurment()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    stop = true;
}
