/**
 * @file 09_integrated_greenhouse.cpp
 * @brief Everything together: the Cell-DEVS floor feeds the DEVS control system.
 *
 * THE PICTURE (one dynamic coupled model, three modelling styles side by side)
 *
 *    +---------------------------- smart_greenhouse (coupled) --------------------------------+
 *    |                                                                                         |
 *    |  floor  (Cell-DEVS lattice, 63 cells)                                                   |
 *    |     cell (4,3) --EOC--> [cell_out]                                                      |
 *    |                             |  IC                                                       |
 *    |                             v                                                           |
 *    |  probe (adapter)  --reading(float)--> thermostat --heater_command--> heater             |
 *    |                                          (custom PDEVS atomic models)    |              |
 *    |  report_clock (stock) --reset_tick--> energy_meter (stock accumulator) <-+ energy_pulse |
 *    +-----------------------------------------------------------------------------------------+
 *
 *   - `floor` is a `grid_coupled` built from data/floor.json (demo 07). The sensor cell (4,3)
 *     is exposed on the lattice boundary with `expose_cell`.
 *   - `probe` converts the cell's `cell_state_message` into a float reading.
 *   - `thermostat`, `heater`: demo 02's models, here with thresholds 17 / 20 degrees.
 *   - `report_clock` + `energy_meter`: the stock models, producing an energy report every 5 s.
 *
 * WHAT HAPPENS: the floor starts at 15 degrees, so the thermostat switches the heater ON at
 * t = 0.5; the floor slowly warms (its own heaters and windows act on it); when the sensor cell
 * passes 20 degrees the thermostat switches the supplementary heater OFF half a second later.
 *
 * NOTE: the supplementary heater does not act back on the floor in this demo (the loop is open).
 * Closing it needs a floor cell that listens to the heater, see README, "ideas to extend".
 */
#include <iostream>
#include <sstream>

#include "greenhouse/check.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/floor/cell_probe.hpp"
#include "greenhouse/floor/floor_coupled.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/systems/dynamic_greenhouse.hpp"
#include "greenhouse/trace.hpp"

#include <cadmium/engine/pdevs_dynamic_runner.hpp>

using namespace greenhouse;
namespace lg = cadmium::logger;
using TIME = double;

struct integrated_tag {};
using integrated_logger = lg::multilogger<
    lg::logger<lg::logger_global_time, trace_formatter<TIME>, memory_sink<integrated_tag>>,
    lg::logger<lg::logger_messages, trace_formatter<TIME>, memory_sink<integrated_tag>>,
    lg::logger<lg::logger_state, trace_formatter<TIME>, memory_sink<integrated_tag>>>;

int main() {
    using namespace cadmium::dynamic::modeling;
    using namespace cadmium::dynamic::translate;
    const cell_position sensor{4, 3};

    // ---- 1. the lattice, from JSON, with the sensor cell exposed ----
    auto floor = std::make_shared<floor_coupled<TIME>>("floor");
    floor->add_lattice_json("data/floor.json");
    floor->couple_cells();
    floor->expose_cell(sensor);

    // ---- 2. the control chain, built with the dynamic API ----
    thermostat_config thermostat_settings;
    thermostat_settings.low_c = 17.0;   // heater ON below 17 degrees ...
    thermostat_settings.high_c = 20.0;  // ... OFF above 20
    auto probe = make_dynamic_atomic_model<cell_probe, TIME>("probe", sensor);
    auto thermo = make_dynamic_atomic_model<thermostat, TIME>("thermostat", thermostat_settings);
    auto heat = make_dynamic_atomic_model<heater, TIME>("heater");
    auto meter = make_dynamic_atomic_model<energy_meter, TIME>("energy_meter");
    auto clock = make_dynamic_atomic_model<report_clock, TIME>("report_clock");

    // ---- 3. couple everything. The `coupled` constructor validates all links. ----
    Models models{floor, probe, thermo, heat, meter, clock};
    ICs ic{make_IC<floor_cell_ports::cell_out, cell_probe_defs::cell_in>("floor", "probe"),
           make_IC<cell_probe_defs::reading_out, thermostat_defs::reading>("probe", "thermostat"),
           make_IC<thermostat_defs::heater_command_out, heater_defs::command>("thermostat", "heater"),
           make_IC<heater_defs::energy_pulse, meter_defs::add>("heater", "energy_meter"),
           make_IC<report_clock_defs::out, meter_defs::reset>("report_clock", "energy_meter")};
    auto top = std::make_shared<coupled<TIME>>("smart_greenhouse", models, Ports{}, Ports{}, EICs{}, EOCs{}, ic);

    // ---- 4. run ----
    section("running the integrated greenhouse for 60 simulated seconds");
    memory_sink<integrated_tag>::clear();
    {
        cadmium::dynamic::engine::runner<TIME, integrated_logger> runner(top, 0.0);
        runner.run_until(60.0);
    }
    const auto trace = parse_trace(memory_sink<integrated_tag>::contents());
    std::cout << "  " << trace.size() << " trace events\n";

    // ---- 5. what do we expect? ----
    // (a) the thermostat switched the heater ON exactly once, at t = 0.5, and OFF exactly once
    double on_time = -1, off_time = -1;
    int on_commands = 0, off_commands = 0;
    for (const auto& e : outputs_of(trace, "thermostat")) {
        if (e.second.find("heater ON") != std::string::npos) { ++on_commands; on_time = e.first; }
        if (e.second.find("heater OFF") != std::string::npos) { ++off_commands; off_time = e.first; }
    }
    std::cout << "  heater switched ON at t=" << on_time << " and OFF at t=" << off_time << "\n";
    CHECK_EQ(on_commands, 1);
    CHECK_EQ(off_commands, 1);
    CHECK_EQ(on_time, 0.5);  // first reading (15) arrives at t=0; decision latency 0.5 s

    // (b) the OFF decision comes after the sensor cell FIRST published more than 20 degrees.
    //     (It can be a little later than 0.5 s after that first reading: the published values
    //      jitter by 0.1 degree, and the decision needs a reading that is still above 20.)
    double crossing = -1;
    for (const auto& e : outputs_of(trace, "probe")) {
        const auto open = e.second.find('{');
        if (open != std::string::npos && std::atof(e.second.c_str() + open + 1) > 20.0) { crossing = e.first; break; }
    }
    std::cout << "  sensor first above 20 degrees at t=" << crossing << "; OFF decision at t=" << off_time << "\n";
    CHECK(crossing > 0);
    CHECK(off_time >= crossing);
    CHECK(off_time < crossing + 5.0);
    // the OFF command carries the reading that caused it: it must be above the 20 degree limit
    double off_reading = 0;
    for (const auto& e : outputs_of(trace, "thermostat")) {
        const auto at = e.second.find("heater OFF (reading ");
        if (at != std::string::npos) off_reading = std::atof(e.second.c_str() + at + 20);
    }
    CHECK(off_reading > 20.0);

    // (c) the heater pulses once a second from t=1.5 until the OFF command, then never again
    auto pulses = outputs_of(trace, "heater");
    CHECK(!pulses.empty());
    CHECK_EQ(pulses.front().first, 1.5);
    for (const auto& p : pulses) CHECK_MSG(p.first <= off_time + 1e-9, "no pulse after the OFF command (t=" + std::to_string(p.first) + ")");
    const std::string heater_final = last_state_of(trace, "heater");
    std::cout << "  heater final state: " << heater_final << "\n";
    CHECK_EQ(pulses.size(), static_cast<std::size_t>(std::atoi(heater_final.substr(heater_final.find("pulses=") + 7).c_str())));

    // (d) energy accounting: every pulse is either in a 5-second report or still in the meter's sum
    int reported = 0;
    for (const auto& r : reports_of(trace, "energy_meter")) reported += r.second;
    const std::string meter_final = last_state_of(trace, "energy_meter");  // "[sum, on_reset]"
    const int residual = std::atoi(meter_final.c_str() + 1);
    std::cout << "  energy: " << reported << " units reported + " << residual << " pending = " << pulses.size()
              << " pulses emitted\n";
    CHECK_EQ(reported + residual, static_cast<int>(pulses.size()));

    // (e) the lattice itself behaved like in demo 07 (same JSON, same time horizon)
    auto temps = floor->snapshot();
    CHECK_EQ(temps.size(), 63u);
    CHECK(temps.at(sensor) > 20.0 && temps.at(sensor) < 23.0);
    CHECK_EQ((temps.at({7, 3})), 40.0);
    std::cout << "  floor sensor cell (4,3) at t=60: " << temps.at(sensor) << " degrees\n";

    return check_summary("09_integrated_greenhouse");
}
