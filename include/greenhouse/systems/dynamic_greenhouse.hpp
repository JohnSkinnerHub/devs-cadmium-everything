/**
 * @file dynamic_greenhouse.hpp
 * @brief The same greenhouse, assembled at RUN TIME with Cadmium's dynamic modeling API.
 *
 * TWO WAYS TO GET A DYNAMIC MODEL
 *
 *   (A) TRANSLATE a static model.
 *         auto top = cadmium::dynamic::translate::make_dynamic_coupled_model<TIME, greenhouse_top>();
 *       Cadmium walks the static type, creates one dynamic atomic per sub-model (default
 *       constructed, id = the C++ type name) and rebuilds the EIC/EOC/IC lists. No extra code,
 *       but the models can only be default-constructed.
 *
 *   (B) BUILD it by hand (`build_dynamic_greenhouse` below). You choose
 *         - the model ids,                   -> readable logs
 *         - constructor arguments,           -> one model class, many parameterisations
 *         - the structure itself,            -> could come from a file, a GUI, a loop...
 *       Atomic models are created with `make_dynamic_atomic_model<ATOMIC, TIME>(id, args...)`,
 *       links with `make_EIC / make_EOC / make_IC<PORT_FROM, PORT_TO>(ids...)`, and coupled
 *       models with the `dynamic::modeling::coupled<TIME>` constructors.
 *
 * HOW THE DYNAMIC MODEL WORKS (the key idea)
 *   Static Cadmium knows every port as a C++ type at compile time. The dynamic engine erases
 *   those types: ports are identified by their `std::type_index`, message bags travel in a
 *   `std::map<type_index, boost::any>` and each coupling is a `link<PORT_FROM, PORT_TO>` object
 *   that still remembers the real types (so it can `any_cast` the bag back). A link therefore
 *   keeps the compile-time guarantee that the two ports carry the same message type
 *   (`static_assert` in `link`), while the *wiring* is decided at run time.
 *   Because wiring is data, it can be validated at run time too: the `coupled` constructors
 *   throw `std::domain_error` if a link names a model or a port that does not exist.
 *
 * DYNAMIC IS ALSO WHAT CELL-DEVS IS BUILT ON (see floor/): a lattice whose size comes from a
 * JSON file cannot be a template argument.
 */
#ifndef GREENHOUSE_SYSTEMS_DYNAMIC_GREENHOUSE_HPP
#define GREENHOUSE_SYSTEMS_DYNAMIC_GREENHOUSE_HPP

#include <map>
#include <memory>
#include <string>
#include <typeindex>

// stock_aliases.hpp first: it pulls in tuple_to_ostream.hpp before any dynamic header
#include "../models/stock_aliases.hpp"

#include <cadmium/modeling/dynamic_atomic.hpp>
#include <cadmium/modeling/dynamic_coupled.hpp>
#include <cadmium/modeling/dynamic_model_translator.hpp>

#include "../models/heater.hpp"
#include "../models/thermostat.hpp"
#include "static_greenhouse.hpp"

namespace greenhouse {

/// Parameters that can only be given when a model is built by hand (static default-constructs).
struct greenhouse_options {
    thermostat_config thermostat{};   ///< thresholds and decision delay of the thermostat
    heater_config heater{};           ///< heater pulse period
    std::string feed_path = "data/sensor_feed.txt";  ///< the file the sensor feed replays
};

/// A dynamic coupled model plus handles on its atomic models (so that tests can inspect them).
template <typename TIME>
struct dynamic_greenhouse {
    std::shared_ptr<cadmium::dynamic::modeling::coupled<TIME>> top;
    std::map<std::string, std::shared_ptr<cadmium::dynamic::modeling::atomic_abstract<TIME>>> atoms;
};

/// Recursively collects every atomic model below `coupled`, indexed by id.
template <typename TIME>
std::map<std::string, std::shared_ptr<cadmium::dynamic::modeling::atomic_abstract<TIME>>>
collect_atomic_models(const std::shared_ptr<cadmium::dynamic::modeling::coupled<TIME>>& coupled) {
    std::map<std::string, std::shared_ptr<cadmium::dynamic::modeling::atomic_abstract<TIME>>> atoms;
    for (const auto& m : coupled->_models) {
        if (auto inner = std::dynamic_pointer_cast<cadmium::dynamic::modeling::coupled<TIME>>(m)) {
            auto below = collect_atomic_models<TIME>(inner);
            atoms.insert(below.begin(), below.end());
        } else if (auto atom = std::dynamic_pointer_cast<cadmium::dynamic::modeling::atomic_abstract<TIME>>(m)) {
            atoms[atom->get_id()] = atom;
        }
    }
    return atoms;
}

/// (A) Translate the static `greenhouse_top` into a dynamic model.
template <typename TIME>
dynamic_greenhouse<TIME> translate_static_greenhouse() {
    dynamic_greenhouse<TIME> g;
    g.top = cadmium::dynamic::translate::make_dynamic_coupled_model<TIME, greenhouse_top>();
    g.atoms = collect_atomic_models<TIME>(g.top);
    return g;
}

/// The model ids used by `build_dynamic_greenhouse` (hand-chosen, human readable).
namespace ids {
inline const char* const pulse_clock = "pulse_clock";
inline const char* const report_clock = "report_clock";
inline const char* const water_meter = "water_meter";
inline const char* const sensor_feed = "sensor_feed";
inline const char* const thermostat = "thermostat";
inline const char* const heater = "heater";
inline const char* const energy_meter = "energy_meter";
inline const char* const alarm_latch = "alarm_latch";
inline const char* const alarm_panel = "alarm_panel";
inline const char* const irrigation = "irrigation";
inline const char* const climate = "climate";
inline const char* const greenhouse = "greenhouse";
}  // namespace ids

/// (B) Build the climate subsystem by hand. Used both inside `build_dynamic_greenhouse` and,
/// on its own, to show `run_until_passivate` with the dynamic runner.
template <typename TIME>
std::shared_ptr<cadmium::dynamic::modeling::coupled<TIME>> build_dynamic_climate(
        const greenhouse_options& opt,
        std::map<std::string, std::shared_ptr<cadmium::dynamic::modeling::atomic_abstract<TIME>>>& atoms) {
    using namespace cadmium::dynamic::modeling;
    using namespace cadmium::dynamic::translate;

    // ---- atomic models: id + constructor arguments forwarded to the model's constructor ----
    // (`make_dynamic_atomic_model<ATOMIC, TIME>(id, args...)` -> shared_ptr<atomic_abstract<TIME>>)
    auto feed = make_dynamic_atomic_model<sensor_feed, TIME>(ids::sensor_feed, opt.feed_path);
    auto thermo = make_dynamic_atomic_model<thermostat, TIME>(ids::thermostat, opt.thermostat);
    auto heat = make_dynamic_atomic_model<heater, TIME>(ids::heater, opt.heater);
    auto meter = make_dynamic_atomic_model<energy_meter, TIME>(ids::energy_meter);  // no args: default ctor
    auto latch = make_dynamic_atomic_model<alarm_latch, TIME>(ids::alarm_latch);
    auto panel = make_dynamic_atomic_model<alarm_panel, TIME>(ids::alarm_panel);
    for (auto* a : {&feed, &thermo, &heat, &meter, &latch, &panel}) atoms[(*a)->get_id()] = *a;

    // ---- the coupled model ----
    // Passing named containers selects the `coupled(id, Models, Ports, Ports, EICs, EOCs, ICs)`
    // constructor. The constructor validates every link (throws std::domain_error otherwise).
    Models models{feed, thermo, heat, meter, latch, panel};
    Ports input_ports{typeid(climate_ports::report_tick_in)};
    Ports output_ports{typeid(climate_ports::energy_report), typeid(climate_ports::alarm_raised)};
    EICs eic{make_EIC<climate_ports::report_tick_in, meter_defs::reset>(ids::energy_meter)};
    EOCs eoc{make_EOC<meter_defs::sum, climate_ports::energy_report>(ids::energy_meter),
             make_EOC<alarm_latch_defs::out, climate_ports::alarm_raised>(ids::alarm_latch)};
    ICs ic{make_IC<sensor_feed_defs::out, thermostat_defs::reading>(ids::sensor_feed, ids::thermostat),
           make_IC<thermostat_defs::heater_command_out, heater_defs::command>(ids::thermostat, ids::heater),
           make_IC<heater_defs::energy_pulse, meter_defs::add>(ids::heater, ids::energy_meter),
           make_IC<thermostat_defs::overheat_alarm, alarm_latch_defs::in>(ids::thermostat, ids::alarm_latch),
           make_IC<alarm_latch_defs::out, alarm_panel_defs::in>(ids::alarm_latch, ids::alarm_panel)};
    return std::make_shared<coupled<TIME>>(ids::climate, models, input_ports, output_ports, eic, eoc, ic);
}

/// (B) Build the whole greenhouse by hand.
template <typename TIME>
dynamic_greenhouse<TIME> build_dynamic_greenhouse(const greenhouse_options& opt = greenhouse_options{}) {
    using namespace cadmium::dynamic::modeling;
    using namespace cadmium::dynamic::translate;
    dynamic_greenhouse<TIME> g;

    // ---- irrigation: built with the *initializer-list* constructor of coupled<TIME> ----
    // Braced arguments select `coupled(id, initializer_list<...>, ...)`; this is the same
    // information as the vectors above, just written inline.
    auto pulse = make_dynamic_atomic_model<pulse_clock, TIME>(ids::pulse_clock);
    auto report = make_dynamic_atomic_model<report_clock, TIME>(ids::report_clock);
    auto water = make_dynamic_atomic_model<water_meter, TIME>(ids::water_meter);
    for (auto* a : {&pulse, &report, &water}) g.atoms[(*a)->get_id()] = *a;
    auto irrigation = std::make_shared<coupled<TIME>>(
        ids::irrigation,
        initializer_list_Models{pulse, report, water},
        initilizer_list_Ports{},  // (sic) Cadmium spells the input-ports alias "initilizer"
        initilizer_list_Ports{typeid(irrigation_ports::water_report), typeid(irrigation_ports::report_tick)},
        initializer_list_EICs{},
        initializer_list_EOCs{make_EOC<meter_defs::sum, irrigation_ports::water_report>(ids::water_meter),
                              make_EOC<report_clock_defs::out, irrigation_ports::report_tick>(ids::report_clock)},
        initializer_list_ICs{make_IC<pulse_clock_defs::out, meter_defs::add>(ids::pulse_clock, ids::water_meter),
                             make_IC<report_clock_defs::out, meter_defs::reset>(ids::report_clock, ids::water_meter)});

    // ---- climate: vector constructor, see above ----
    auto climate = build_dynamic_climate<TIME>(opt, g.atoms);

    // ---- top: two coupled models as sub-models (hierarchy works exactly like in the static case)
    Models models{irrigation, climate};
    Ports no_ports_;
    Ports outputs{typeid(greenhouse_ports::water_report), typeid(greenhouse_ports::energy_report),
                  typeid(greenhouse_ports::alarm)};
    EICs eic;
    EOCs eoc{make_EOC<irrigation_ports::water_report, greenhouse_ports::water_report>(ids::irrigation),
             make_EOC<climate_ports::energy_report, greenhouse_ports::energy_report>(ids::climate),
             make_EOC<climate_ports::alarm_raised, greenhouse_ports::alarm>(ids::climate)};
    ICs ic{make_IC<irrigation_ports::report_tick, climate_ports::report_tick_in>(ids::irrigation, ids::climate)};
    g.top = std::make_shared<coupled<TIME>>(ids::greenhouse, models, no_ports_, outputs, eic, eoc, ic);
    return g;
}

}  // namespace greenhouse

#endif  // GREENHOUSE_SYSTEMS_DYNAMIC_GREENHOUSE_HPP
