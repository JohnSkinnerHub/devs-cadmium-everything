/**
 * @file static_greenhouse.hpp
 * @brief The greenhouse control system as a *static* (compile-time) hierarchical coupled model.
 *
 * THE PICTURE
 *
 *   greenhouse_top
 *   |
 *   +-- irrigation  (coupled)                          +-- climate  (coupled)
 *   |     pulse_clock  --add-->  water_meter           |     sensor_feed --reading--> thermostat
 *   |     report_clock --reset-> water_meter           |     thermostat --heater_command--> heater
 *   |     water_meter  --sum---> [water_report]        |     heater --energy_pulse--> energy_meter
 *   |     report_clock --------> [report_tick]--+      |     thermostat --overheat_alarm--> alarm_latch
 *   |                                           |      |     alarm_latch --> alarm_panel  (sink)
 *   |                                           |      |     [report_tick_in] --reset--> energy_meter
 *   |                                           +----> |     energy_meter --sum--> [energy_report]
 *   |                                                  |     alarm_latch ---------> [alarm_raised]
 *   +-- top outputs: water_report, energy_report, alarm
 *
 *   Every [name] is a port of a coupled model. Couplings come in three kinds (DEVS formalism):
 *     EIC  external input coupling   coupled model input port  -> sub-model input port
 *     EOC  external output coupling  sub-model output port     -> coupled model output port
 *     IC   internal coupling         sub-model output port     -> another sub-model's input port
 *
 * WHY STATIC
 *   In Cadmium's static style the *whole structure is a type*: ports, sub-models and couplings
 *   are template arguments. The compiler therefore validates the model (port kinds, message
 *   types, existence of ports, no self-coupling) and the engine is generated with zero virtual
 *   calls. The price: the structure cannot be decided at run time. For run-time structure
 *   (e.g. read from JSON) Cadmium offers the dynamic style, see dynamic_greenhouse.hpp, and
 *   `dynamic::translate::make_dynamic_coupled_model` can even convert this static model into a
 *   dynamic one.
 *
 * TIME
 *   Every model here is a `template<typename TIME>`. The top model is also a template so the
 *   runner can instantiate it with float, double or `greenhouse::fixed_time`.
 *
 * READING THE TYPES
 *   `coupled_model<TIME, InputPorts, OutputPorts, Submodels, EICs, EOCs, ICs>` is the
 *   Cadmium encoding of the DEVS coupled model  N = < X, Y, D, {M_d}, EIC, EOC, IC >.
 *   (Parallel DEVS has no SELECT function: simultaneous events are all processed together.
 *    The *classic* DEVS `coupling` type, which does have one, is shown in the classic demo.)
 */
#ifndef GREENHOUSE_SYSTEMS_STATIC_GREENHOUSE_HPP
#define GREENHOUSE_SYSTEMS_STATIC_GREENHOUSE_HPP

#include <tuple>

#include <cadmium/concept/coupled_model_assert.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../models/heater.hpp"
#include "../models/stock_aliases.hpp"
#include "../models/thermostat.hpp"

namespace greenhouse {

using cadmium::modeling::EIC;
using cadmium::modeling::EOC;
using cadmium::modeling::IC;
using cadmium::modeling::models_tuple;
using cadmium::modeling::pdevs::coupled_model;

using no_ports = std::tuple<>;       // "this coupled model has no input (or output) ports"
using no_couplings = std::tuple<>;   // "...and no couplings of this kind"

// =========================================================================================
// irrigation: count watering pulses and report the total every five seconds
// (Cadmium's own "count fives" example, wrapped into a reusable coupled model)
// =========================================================================================
struct irrigation_ports {
    struct water_report : public cadmium::out_port<int> {};
    struct report_tick : public cadmium::out_port<report_tick_msg> {};
};

template <typename TIME>
using irrigation_model = coupled_model<
    TIME,
    no_ports,                                                                   // X: no inputs
    std::tuple<irrigation_ports::water_report, irrigation_ports::report_tick>,  // Y
    models_tuple<pulse_clock, report_clock, water_meter>,                       // D
    no_couplings,                                                               // EIC
    std::tuple<                                                                 // EOC
        EOC<water_meter, meter_defs::sum, irrigation_ports::water_report>,
        EOC<report_clock, report_clock_defs::out, irrigation_ports::report_tick>>,
    std::tuple<                                                                 // IC
        IC<pulse_clock, pulse_clock_defs::out, water_meter, meter_defs::add>,
        IC<report_clock, report_clock_defs::out, water_meter, meter_defs::reset>>>;

// =========================================================================================
// climate: replay sensor readings, decide on heating, meter the energy, latch the alarm
// =========================================================================================
struct climate_ports {
    struct report_tick_in : public cadmium::in_port<report_tick_msg> {};
    struct energy_report : public cadmium::out_port<int> {};
    struct alarm_raised : public cadmium::out_port<int> {};
};

template <typename TIME>
using climate_model = coupled_model<
    TIME,
    std::tuple<climate_ports::report_tick_in>,                                  // X
    std::tuple<climate_ports::energy_report, climate_ports::alarm_raised>,      // Y
    models_tuple<sensor_feed, thermostat, heater, energy_meter, alarm_latch, alarm_panel>,
    std::tuple<                                                                 // EIC
        EIC<climate_ports::report_tick_in, energy_meter, meter_defs::reset>>,
    std::tuple<                                                                 // EOC
        EOC<energy_meter, meter_defs::sum, climate_ports::energy_report>,
        EOC<alarm_latch, alarm_latch_defs::out, climate_ports::alarm_raised>>,
    std::tuple<                                                                 // IC
        IC<sensor_feed, sensor_feed_defs::out, thermostat, thermostat_defs::reading>,
        IC<thermostat, thermostat_defs::heater_command_out, heater, heater_defs::command>,
        IC<heater, heater_defs::energy_pulse, energy_meter, meter_defs::add>,
        IC<thermostat, thermostat_defs::overheat_alarm, alarm_latch, alarm_latch_defs::in>,
        IC<alarm_latch, alarm_latch_defs::out, alarm_panel, alarm_panel_defs::in>>>;

// =========================================================================================
// top: irrigation tells climate when to report, outputs are exposed to the outside world
// =========================================================================================
struct greenhouse_ports {
    struct water_report : public cadmium::out_port<int> {};
    struct energy_report : public cadmium::out_port<int> {};
    struct alarm : public cadmium::out_port<int> {};
};

template <typename TIME>
using greenhouse_top = coupled_model<
    TIME,
    no_ports,
    std::tuple<greenhouse_ports::water_report, greenhouse_ports::energy_report, greenhouse_ports::alarm>,
    models_tuple<irrigation_model, climate_model>,
    no_couplings,
    std::tuple<
        EOC<irrigation_model, irrigation_ports::water_report, greenhouse_ports::water_report>,
        EOC<climate_model, climate_ports::energy_report, greenhouse_ports::energy_report>,
        EOC<climate_model, climate_ports::alarm_raised, greenhouse_ports::alarm>>,
    std::tuple<
        IC<irrigation_model, irrigation_ports::report_tick, climate_model, climate_ports::report_tick_in>>>;

// =========================================================================================
// climate_standalone: the climate subsystem alone, nothing coupled to its ports.
// It has no periodic generator, so after the sensor file is exhausted and the heater is OFF
// every model is passive: the simulation has *nothing left to do*. That makes it the
// right model to demonstrate `runner::run_until_passivate()`, which the full greenhouse
// (with its endless clocks) can never reach.
// =========================================================================================
template <typename TIME>
using climate_standalone = coupled_model<
    TIME, no_ports, no_ports, models_tuple<climate_model>, no_couplings, no_couplings, no_couplings>;

/**
 * Runs Cadmium's compile-time validation of the whole hierarchy.
 * Calling this function is what instantiates the `static_assert`s inside the concept checks:
 *  - every EIC/EOC/IC refers to ports that exist and have the right direction and message type;
 *  - no sub-model is coupled to itself;
 *  - ports are unique inside each model;
 *  - every atomic model has the five DEVS functions with the right signatures.
 * (The engines run the same checks, so calling it explicitly is only to make the point.)
 */
inline void verify_static_greenhouse() {
    cadmium::concept::pdevs::coupled_model_assert<greenhouse_top>();
}

}  // namespace greenhouse

#endif  // GREENHOUSE_SYSTEMS_STATIC_GREENHOUSE_HPP
