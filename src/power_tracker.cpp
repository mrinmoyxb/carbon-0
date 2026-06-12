#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <mach/mach_init.h>
#include <mach/mach_error.h>
#include <mach/mach_host.h>
#include <mach/vm_map.h>

// Constants for our estimation
const double IDLE_WATTAGE = 1.0;     // Estimated idle power for Apple Silicon
const double MAX_CPU_WATTAGE = 15.0; // Estimated max CPU power for Apple Silicon M-series
const double CARBON_INTENSITY_G_PER_KWH = 400.0; // Global average carbon intensity

struct CpuTicks {
    unsigned long long user;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long nice;
};

// Function to get current CPU ticks using macOS Mach kernel APIs
bool getCpuTicks(CpuTicks& ticks) {
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    host_cpu_load_info_data_t cpu_load;
    
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO,
                                         (host_info64_t)&cpu_load, &count);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    
    ticks.user = cpu_load.cpu_ticks[CPU_STATE_USER];
    ticks.system = cpu_load.cpu_ticks[CPU_STATE_SYSTEM];
    ticks.idle = cpu_load.cpu_ticks[CPU_STATE_IDLE];
    ticks.nice = cpu_load.cpu_ticks[CPU_STATE_NICE];
    return true;
}

int main() {
    std::cout << "Starting Carbon Power Tracker (Estimation Mode)..." << std::endl;
    std::cout << "Press Ctrl+C to stop.\n" << std::endl;
    
    CpuTicks prev_ticks = {0, 0, 0, 0};
    getCpuTicks(prev_ticks);
    
    double total_emissions_g = 0.0;
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        CpuTicks curr_ticks;
        if (!getCpuTicks(curr_ticks)) {
            std::cerr << "Failed to get CPU stats" << std::endl;
            continue;
        }
        
        unsigned long long total_user = curr_ticks.user - prev_ticks.user;
        unsigned long long total_system = curr_ticks.system - prev_ticks.system;
        unsigned long long total_idle = curr_ticks.idle - prev_ticks.idle;
        unsigned long long total_nice = curr_ticks.nice - prev_ticks.nice;
        
        unsigned long long total_delta = total_user + total_system + total_idle + total_nice;
        
        double cpu_usage_pct = 0.0;
        if (total_delta > 0) {
            cpu_usage_pct = (double)(total_user + total_system + total_nice) / total_delta;
        }
        
        // 1. Calculate Estimated Wattage
        double current_wattage = IDLE_WATTAGE + (cpu_usage_pct * MAX_CPU_WATTAGE);
        
        // 2. Convert Wattage to kWh for this 1-second interval
        // Energy in Joules = Watts * Seconds (1 sec)
        // 1 kWh = 3.6 million Joules
        double kwh_this_second = current_wattage / 3600000.0; 
        
        // 3. Convert kWh to Grams of CO2
        double co2_this_second = kwh_this_second * CARBON_INTENSITY_G_PER_KWH;
        
        // Accumulate
        total_emissions_g += co2_this_second;
        
        // Print
        std::cout << "\r" 
                  << "CPU Usage: " << std::fixed << std::setprecision(1) << (cpu_usage_pct * 100.0) << "% | "
                  << "Est. Power: " << std::fixed << std::setprecision(2) << current_wattage << " W | "
                  << "Emissions so far: " << std::fixed << std::setprecision(6) << total_emissions_g << " g CO2" 
                  << std::flush;
        
        prev_ticks = curr_ticks;
    }
    
    return 0;
}
