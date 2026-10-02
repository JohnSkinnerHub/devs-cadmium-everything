/**
 * @file 04_engine_internals.cpp
 * @brief Playing "the simulator" ourselves: Cadmium's engine pieces used one at a time.
 *
 * WHY
 *   `runner::run_until()` hides the Parallel-DEVS simulation algorithm. This demo opens the box.
 *   For every pair of engine components we call the individual steps by hand, so a reader sees
 *   exactly what the abstract simulator of the PDEVS formalism does:
 *
 *       init(t0)                         every atomic model: next := t0 + ta(s)
 *       loop:
 *         t := min next event time
 *         collect_outputs(t)             models whose `next == t` evaluate lambda(s)
 *         route messages                 IC: sub-model -> sub-model, EOC: -> outside
 *         advance_simulation(t)          each model with input and/or `next == t` does
 *                                          delta_ext, delta_int or delta_con
 *
 * PART 1  STATIC engine (namespace cadmium::engine)
 *    simulator, coordinator, coordinate_tuple, init_subcoordinators, min_next_in_tuple,
 *    collect_outputs_in_subcoordinators, advance_simulation_in_subengines, get_engine_by_model,
 *    get_engine_type_by_model, route_internal_coupled_messages_on_subcoordinators,
 *    route_external_input_coupled_messages_on_subcoordinators, collect_messages_by_eoc,
 *    all_bags_empty, and the three domain_error situations.
 * PART 2  DYNAMIC engine (namespace cadmium::dynamic::...)
 *    message_bags and its helpers, simulator, coordinator, link, link validation functions,
 *    and the translator's building blocks.
 */
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#include "greenhouse/check.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/systems/dynamic_greenhouse.hpp"
#include "greenhouse/systems/static_greenhouse.hpp"

#include <cadmium/engine/pdevs_coordinator.hpp>
#include <cadmium/engine/pdevs_dynamic_coordinator.hpp>
#include <cadmium/engine/pdevs_dynamic_simulator.hpp>
#include <cadmium/engine/pdevs_simulator.hpp>

using namespace greenhouse;
namespace lg = cadmium::logger;
using cadmium::get_messages;
using cadmium::make_message_bags;

// Each kind of log information goes to its own in-memory sink so we can see what an engine call
// logged. Static and dynamic engines call the same formatter functions with different arguments,
// so the stock `formatter<float>` / `dynamic::logger::formatter<float>` are used as they are.
struct info_tag {};
struct state_tag {};
struct routing_tag {};
struct messages_tag {};
using static_log = lg::multilogger<
    lg::logger<lg::logger_info, lg::formatter<float>, memory_sink<info_tag>>,
    lg::logger<lg::logger_state, lg::formatter<float>, memory_sink<state_tag>>,
    lg::logger<lg::logger_messages, lg::formatter<float>, memory_sink<messages_tag>>,
    lg::logger<lg::logger_message_routing, lg::formatter<float>, memory_sink<routing_tag>>>;
using dynamic_log = lg::multilogger<
    lg::logger<lg::logger_info, cadmium::dynamic::logger::formatter<float>, memory_sink<info_tag>>,
    lg::logger<lg::logger_state, cadmium::dynamic::logger::formatter<float>, memory_sink<state_tag>>,
    lg::logger<lg::logger_messages, cadmium::dynamic::logger::formatter<float>, memory_sink<messages_tag>>,
    lg::logger<lg::logger_message_routing, cadmium::dynamic::logger::formatter<float>, memory_sink<routing_tag>>>;

// thermostat.heater_command_out -> heater.command, as a static coupling type
using IC_thermostat_to_heater_t =
    cadmium::modeling::IC<thermostat, thermostat_defs::heater_command_out, heater, heater_defs::command>;

static void clear_logs() {
    memory_sink<info_tag>::clear();
    memory_sink<state_tag>::clear();
    memory_sink<routing_tag>::clear();
    memory_sink<messages_tag>::clear();
}
static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static bool h_state_contains(const std::shared_ptr<cadmium::dynamic::modeling::atomic_abstract<float>>& m, const std::string& s) {
    return contains(m->model_state_as_string(), s);
}

int main() {
    const float inf = std::numeric_limits<float>::infinity();

    // =====================================================================================
    section("PART 1a. static simulator: ONE atomic model, stepped by hand");
    // =====================================================================================
    {
        clear_logs();
        cadmium::engine::simulator<heater, float, static_log> sim;  // a simulator owns a model instance
        sim.init(0.0f);                                             // next := 0 + ta(s0) = infinity
        CHECK_EQ(sim.next(), inf);
        CHECK(contains(memory_sink<info_tag>::contents(), "Simulator for model greenhouse::heater<float> initialized to time 0"));

        // Give it an input (an "ON" command) and let it process it at t = 1.0 -> delta_ext
        make_message_bags<heater<float>::input_ports>::type in;
        get_messages<heater_defs::command>(in).push_back(heater_command{power::on, 15.0f});
        sim.inbox(in);                 // the setter used by coordinators
        sim.advance_simulation(1.0f);  // elapsed = 1.0 - last(0.0); then next := 1.0 + ta = 2.0
        CHECK_EQ(sim.next(), 2.0f);
        CHECK(contains(memory_sink<state_tag>::contents(), "is ON sigma=1 pulses=0"));
        CHECK(contains(memory_sink<info_tag>::contents(), "advancing simulation from time 0 to 1"));

        // At its next event time the model computes its output lambda(s) ...
        sim.collect_outputs(2.0f);
        CHECK_EQ(get_messages<heater_defs::energy_pulse>(sim.outbox()).front(), 1);
        CHECK(contains(memory_sink<messages_tag>::contents(), "heater_defs::energy_pulse: {1}"));
        // ... and then, with no input, performs its internal transition delta_int
        sim.advance_simulation(2.0f);
        CHECK_EQ(sim.next(), 3.0f);
        CHECK(contains(memory_sink<state_tag>::contents(), "pulses=1"));

        // At a time BEFORE its next event, collect_outputs yields an EMPTY outbox (no event due)
        sim.collect_outputs(2.5f);
        CHECK(get_messages<heater_defs::energy_pulse>(sim.outbox()).empty());

        // The public inbox/outbox members (`_inbox`, `_outbox`) are what the helpers below use
        CHECK(cadmium::engine::all_bags_empty(sim._inbox));  // no pending input
        get_messages<heater_defs::command>(sim._inbox).push_back(heater_command{power::off, 1.0f});
        CHECK(!cadmium::engine::all_bags_empty(sim._inbox));

        // ---- the three situations a simulator refuses (std::domain_error) ----
        // (1) asking for output after the model's next event time
        CHECK_THROWS(sim.collect_outputs(3.5f), std::domain_error);
        // (2) advancing to a time before the last transition
        CHECK_THROWS(sim.advance_simulation(1.5f), std::domain_error);
        // (3) advancing beyond the next internal event (an event would be skipped)
        CHECK_THROWS(sim.advance_simulation(3.5f), std::domain_error);
        std::cout << "  (1) output after next event, (2) transition in the past, (3) skipping an event: all rejected\n";
    }

    // =====================================================================================
    section("PART 1b. static coordinator: a coupled model stepped by hand (what runner does)");
    // =====================================================================================
    {
        clear_logs();
        // a coordinator owns the simulators/coordinators of all its sub-models
        cadmium::engine::coordinator<irrigation_model, float, static_log> coord;
        coord.init(0.0f);  // initialises every sub-model, next := min of their next events
        CHECK_EQ(coord.next(), 1.0f);

        int reports = 0, report_ticks = 0;
        float last_report = 0;
        // This loop is the body of `runner::run_until`:
        while (coord.next() <= 5.0f) {
            const float t = coord.next();
            coord.collect_outputs(t);                              // lambda of every imminent model; EOC routing
            const auto& out = coord.outbox();                      // what leaves the coupled model right now
            if (!get_messages<irrigation_ports::water_report>(out).empty()) {
                ++reports;
                last_report = get_messages<irrigation_ports::water_report>(out).front();
            }
            if (!get_messages<irrigation_ports::report_tick>(out).empty()) ++report_ticks;
            coord.advance_simulation(t);                           // IC/EIC routing, then delta_ext/int/con
        }
        // At t=5 there are TWO passes over the same time: the report tick (pass 1) makes the water
        // meter schedule its report with ta = 0 (pass 2).
        CHECK_EQ(report_ticks, 1);
        CHECK_EQ(reports, 1);
        CHECK_EQ(last_report, 5.0f);
        CHECK(contains(memory_sink<routing_tag>::contents(), "IC for model"));
        CHECK(contains(memory_sink<routing_tag>::contents(), "EOC for model"));
        // (a static coupled model is logged under its full C++ type name, which is enormous: shown truncated.
        //  The dynamic builder lets you choose readable ids instead, see demo 03.)
        std::cout << "  routing log excerpt: "
                  << memory_sink<routing_tag>::contents().substr(0, 60) << "...\n";

        // ---- an EXTERNAL input into a coupled model (EIC): drive `climate` the way a parent would ----
        clear_logs();
        cadmium::engine::coordinator<climate_model, float, static_log> climate;
        climate.init(0.0f);
        climate.collect_outputs(0.0f);   // the sensor feed's initial (empty) event at t=0
        climate.advance_simulation(0.0f);
        CHECK_EQ(climate.next(), 1.0f);
        // a report tick arrives from outside at t = 0.5, before the model's own next event (1.0)
        get_messages<climate_ports::report_tick_in>(climate._inbox).push_back(report_tick_msg{});
        climate.advance_simulation(0.5f);  // EIC routes it to the energy meter; the meter reacts (delta_ext)
        CHECK_EQ(climate.next(), 0.5f);    // the meter now wants to report immediately (ta = 0)
        climate.collect_outputs(0.5f);
        CHECK_EQ(get_messages<climate_ports::energy_report>(climate.outbox()).front(), 0);  // EOC: sum = 0
        CHECK(contains(memory_sink<routing_tag>::contents(), "EIC for model"));
        std::cout << "  EIC delivered the report tick: the energy meter reported "
                  << get_messages<climate_ports::energy_report>(climate.outbox()).front() << " units\n";
    }

    // =====================================================================================
    section("PART 1c. the coordinator's building blocks, one by one");
    // =====================================================================================
    {
        clear_logs();
        using namespace cadmium::engine;
        // coordinate_tuple<TIME, models-template, LOGGER>::type = the tuple of simulators/coordinators
        // that a coordinator would create for these sub-models.
        using sub_models = coordinate_tuple<float, irrigation_model<float>::models, static_log>::type;
        static_assert(std::tuple_size<sub_models>::value == 3, "three sub-models");
        // (engines are compared through `model_type`, the model they simulate: the engine types
        //  themselves are built from an internal alias template and are not literally equal)
        static_assert(std::is_same<std::tuple_element<2, sub_models>::type::model_type, water_meter<float>>::value,
                      "the third sub-model of irrigation is the water meter");
        using top_subs = coordinate_tuple<float, greenhouse_top<float>::models, static_log>::type;
        static_assert(std::is_same<std::tuple_element<0, top_subs>::type::model_type, irrigation_model<float>>::value,
                      "a coupled sub-model gets a coordinator whose model_type is the coupled model");
        CHECK_MSG(true, "coordinate_tuple: one engine per sub-model, atomic -> simulator, coupled -> coordinator (static_assert)");

        // Work on a hand-made tuple of two simulators: thermostat -> heater
        using pair_t = std::tuple<simulator<thermostat, float, static_log>, simulator<heater, float, static_log>>;
        pair_t pair;
        init_subcoordinators<float>(0.0f, pair);          // init() on every element
        CHECK_EQ(min_next_in_tuple(pair), inf);            // both passive
        // look an engine up by the MODEL type it simulates
        auto& thermo_sim = get_engine_by_model<thermostat<float>>(pair);
        auto& heater_sim = get_engine_by_model<heater<float>>(pair);
        static_assert(std::is_same<get_engine_type_by_model<heater<float>, pair_t>::type,
                                   simulator<heater, float, static_log>>::value, "type-level lookup");
        static_assert(std::is_same<get_engine_type_by_model<irrigation_model<float>, pair_t>::type, NO_SIMULATOR>::value,
                      "a model that is not in the tuple maps to NO_SIMULATOR");
        CHECK_MSG(true, "get_engine_by_model / get_engine_type_by_model (NO_SIMULATOR for unknown models)");

        // (1) give the thermostat a reading and advance: it starts deciding, due at t = 0.5
        get_messages<thermostat_defs::reading>(thermo_sim._inbox).push_back(15.0f);
        advance_simulation_in_subengines<float>(0.0f, pair);
        CHECK_EQ(min_next_in_tuple(pair), 0.5f);
        // (2) collect the outputs of every imminent model at t = 0.5
        collect_outputs_in_subcoordinators<float>(0.5f, pair);
        CHECK_EQ(get_messages<thermostat_defs::heater_command_out>(thermo_sim._outbox).size(), 1u);
        // (3) route thermostat.heater_command_out -> heater.command  (an IC)
        using ics = std::tuple<IC_thermostat_to_heater_t>;
        CHECK(all_bags_empty(heater_sim._inbox));
        route_internal_coupled_messages_on_subcoordinators<float, pair_t, ics, static_log>(0.5f, pair);
        CHECK(!all_bags_empty(heater_sim._inbox));
        CHECK(contains(memory_sink<routing_tag>::contents(), "heater ON (reading 15)"));
        // (4) advance everybody to 0.5: thermostat does delta_int, heater does delta_ext
        advance_simulation_in_subengines<float>(0.5f, pair);
        CHECK_EQ(heater_sim.next(), 1.5f);  // heater is ON: first pulse one second later
        CHECK(thermo_sim.next() == inf);

        // collect_messages_by_eoc: gather sub-model outputs into the coupled model's output bags
        struct external_alarm : public cadmium::out_port<int> {};
        using eoc = std::tuple<cadmium::modeling::EOC<thermostat, thermostat_defs::overheat_alarm, external_alarm>>;
        using outer_out = make_message_bags<std::tuple<external_alarm>>::type;
        get_messages<thermostat_defs::overheat_alarm>(thermo_sim._outbox).push_back(1);  // pretend it alarmed
        auto collected = collect_messages_by_eoc<float, eoc, outer_out, pair_t, static_log>(pair);
        CHECK_EQ(get_messages<external_alarm>(collected).front(), 1);

        // route_external_input_coupled_messages_on_subcoordinators: outside input -> sub-model (an EIC)
        struct external_reading : public cadmium::in_port<float> {};
        using eic = std::tuple<cadmium::modeling::EIC<external_reading, thermostat, thermostat_defs::reading>>;
        using outer_in = make_message_bags<std::tuple<external_reading>>::type;
        outer_in outside;
        get_messages<external_reading>(outside).push_back(22.0f);
        get_messages<thermostat_defs::reading>(thermo_sim._inbox).clear();
        route_external_input_coupled_messages_on_subcoordinators<float, outer_in, pair_t, eic, static_log>(1.0f, outside, pair);
        CHECK_EQ(get_messages<thermostat_defs::reading>(thermo_sim._inbox).front(), 22.0f);
    }

    // =====================================================================================
    section("PART 2a. dynamic message bags: std::map<type_index, boost::any>");
    // =====================================================================================
    using namespace cadmium::dynamic::modeling;
    using heater_in_bags = make_message_bags<heater<float>::input_ports>::type;
    using heater_out_bags = make_message_bags<heater<float>::output_ports>::type;
    {
        // The dynamic engine cannot name port types at run time, so bags travel type-erased.
        cadmium::dynamic::message_bags empty = create_empty_message_bags<heater_in_bags>();
        CHECK_EQ(empty.size(), 1u);  // one (empty) bag per port, keyed by the port's type_index
        CHECK(empty.count(typeid(heater_defs::command)) == 1);
        CHECK(cadmium::dynamic::engine::all_bags_empty<heater_in_bags>(empty));

        // fill a tuple of bags from the map and the map from a tuple (the two conversions the
        // dynamic atomic wrapper performs around every transition and output call)
        cadmium::dynamic::message_bags map;
        map[typeid(heater_defs::command)] = cadmium::message_bag<heater_defs::command>{heater_command{power::on, 12.0f}};
        heater_in_bags tuple_bags;
        fill_bags_from_map(map, tuple_bags);
        CHECK_EQ(get_messages<heater_defs::command>(tuple_bags).front().because_of, 12.0f);
        CHECK(!cadmium::dynamic::engine::all_bags_empty<heater_in_bags>(map));

        heater_out_bags out_tuple;
        get_messages<heater_defs::energy_pulse>(out_tuple).push_back(1);
        cadmium::dynamic::message_bags out_map;
        fill_map_from_bags(out_tuple, out_map);
        CHECK_EQ(boost::any_cast<cadmium::message_bag<heater_defs::energy_pulse>&>(out_map.at(typeid(heater_defs::energy_pulse))).messages.size(), 1u);

        std::ostringstream oss;
        print_dynamic_messages_by_port<heater<float>::output_ports>(oss, out_map);
        std::cout << "  printed by port: " << oss.str() << "\n";
        CHECK_EQ(oss.str(), std::string("[greenhouse::heater_defs::energy_pulse: {1}]"));

        // port lists as type_index vectors (what `Ports` is)
        Ports p = create_dynamic_ports<heater<float>::input_ports>();
        CHECK_EQ(p.size(), 1u);
        CHECK(is_in(typeid(heater_defs::command), p));
        CHECK(!is_in(typeid(heater_defs::energy_pulse), p));
    }

    // =====================================================================================
    section("PART 2b. dynamic simulator and coordinator, stepped by hand");
    // =====================================================================================
    {
        clear_logs();
        auto h = cadmium::dynamic::translate::make_dynamic_atomic_model<heater, float>("my_heater");
        cadmium::dynamic::engine::simulator<float, dynamic_log> sim(h);
        sim.init(0.0f);
        CHECK_EQ(sim.get_model_id(), std::string("my_heater"));
        CHECK_EQ(sim.next(), inf);
        // inbox() gives access to the type-erased input bags. The engine decides "there is input"
        // by looking at whether the MAP is empty, so only insert a bag when there is something in it.
        sim.inbox()[typeid(heater_defs::command)] = cadmium::message_bag<heater_defs::command>{heater_command{power::on, 15.0f}};
        sim.advance_simulation(1.0f);
        CHECK_EQ(sim.next(), 2.0f);
        sim.collect_outputs(2.0f);
        CHECK_EQ(boost::any_cast<cadmium::message_bag<heater_defs::energy_pulse>&>(
                     sim.outbox().at(typeid(heater_defs::energy_pulse))).messages.front(), 1);
        CHECK_EQ(h->model_state_as_string(), std::string("ON sigma=1 pulses=0"));
        CHECK_THROWS(sim.collect_outputs(3.0f), std::domain_error);
        CHECK_THROWS(sim.advance_simulation(0.5f), std::domain_error);
        CHECK_THROWS(sim.advance_simulation(2.5f), std::domain_error);

        // The abstract model interface that the engine talks to:
        CHECK_EQ(h->get_id(), std::string("my_heater"));
        CHECK_EQ(h->get_input_ports().size(), 1u);
        CHECK_EQ(h->get_output_ports().size(), 1u);
        CHECK_EQ(h->time_advance(), 1.0f);
        sim.advance_simulation(2.0f);  // no input and next == 2.0: delta_int
        CHECK_EQ(h->model_state_as_string(), std::string("ON sigma=1 pulses=1"));
        CHECK_EQ(sim.next(), 3.0f);
        // the three refusals, as in the static simulator
        CHECK_THROWS(sim.collect_outputs(3.5f), std::domain_error);
        CHECK_THROWS(sim.advance_simulation(1.5f), std::domain_error);
        CHECK_THROWS(sim.advance_simulation(3.5f), std::domain_error);

        // GOTCHA: an inbox that contains an EMPTY bag still counts as "input available"
        // (the engine tests whether the map is empty, not whether the bags are).
        auto g2 = cadmium::dynamic::translate::make_dynamic_atomic_model<heater, float>("gotcha_heater");
        cadmium::dynamic::engine::simulator<float, dynamic_log> sim2(g2);
        sim2.init(0.0f);
        sim2.inbox()[typeid(heater_defs::command)] = cadmium::message_bag<heater_defs::command>{heater_command{power::on, 1.0f}};
        sim2.advance_simulation(1.0f);  // heater ON, first pulse due at 2.0
        sim2.inbox()[typeid(heater_defs::command)] = cadmium::message_bag<heater_defs::command>{};  // empty bag!
        sim2.advance_simulation(1.4f);  // still processed as an external transition, with e = 0.4
        CHECK_EQ(g2->model_state_as_string(), std::string("ON sigma=0.6 pulses=0"));
        std::cout << "  (an empty bag in the inbox still triggered an external transition: " << g2->model_state_as_string() << ")\n";

        // ---- a dynamic coordinator over a hand-built coupled model ----
        clear_logs();
        auto g = build_dynamic_greenhouse<float>();
        cadmium::dynamic::engine::coordinator<float, dynamic_log> top(g.top);
        top.init(0.0f);
        CHECK_EQ(top.get_model_id(), std::string(ids::greenhouse));
        CHECK_EQ(top.next(), 0.0f);
        int water_reports = 0;
        while (top.next() <= 5.0f) {
            const float t = top.next();
            top.collect_outputs(t);
            auto it = top.outbox().find(typeid(greenhouse_ports::water_report));
            if (it != top.outbox().end() &&
                !boost::any_cast<cadmium::message_bag<greenhouse_ports::water_report>&>(it->second).messages.empty())
                ++water_reports;
            top.advance_simulation(t);
        }
        CHECK_EQ(water_reports, 1);
        CHECK(contains(memory_sink<routing_tag>::contents(), "routed from"));
        // `inbox()` of a coordinator is where a parent would drop messages for the EIC links.
        CHECK(top.inbox().empty());
    }

    // =====================================================================================
    section("PART 2b2. the dynamic coordinator's building blocks (the vector-of-engines versions)");
    // =====================================================================================
    {
        clear_logs();
        using namespace cadmium::dynamic::engine;
        auto thermo = cadmium::dynamic::translate::make_dynamic_atomic_model<thermostat, float>("thermo");
        auto heat = cadmium::dynamic::translate::make_dynamic_atomic_model<heater, float>("heat");
        auto thermo_sim = std::make_shared<simulator<float, dynamic_log>>(thermo);
        auto heater_sim = std::make_shared<simulator<float, dynamic_log>>(heat);
        // a coordinator's sub-engines are a vector of shared_ptr<engine<TIME>>
        subcoordinators_type<float> engines{thermo_sim, heater_sim};
        // every element is seen through the abstract `engine<TIME>` interface (init, next, inbox, ... + get_model_id)
        std::shared_ptr<cadmium::dynamic::engine::engine<float>> as_engine = thermo_sim;
        CHECK_EQ(as_engine->get_model_id(), std::string("thermo"));
        init_subcoordinators<float>(0.0f, engines);
        CHECK_EQ(min_next_in_subcoordinators<float>(engines), inf);

        // the couplings between engines are pairs (engines, links)
        internal_couplings<float> ics;
        internal_coupling<float> ic;
        ic.first.first = thermo_sim;    // from
        ic.first.second = heater_sim;   // to
        ic.second.push_back(cadmium::dynamic::translate::make_link<thermostat_defs::heater_command_out, heater_defs::command>());
        ics.push_back(ic);
        external_couplings<float> eocs;  // an EOC: (engine, links), here exposing the thermostat's alarm port
        external_coupling<float> eoc;
        eoc.first = thermo_sim;
        eoc.second.push_back(cadmium::dynamic::translate::make_link<thermostat_defs::overheat_alarm, thermostat_defs::overheat_alarm>());
        eocs.push_back(eoc);

        // (1) a reading arrives at the thermostat; advance everything to t = 0
        thermo_sim->inbox()[typeid(thermostat_defs::reading)] = cadmium::message_bag<thermostat_defs::reading>{15.0f};
        advance_simulation_in_subengines<float>(0.0f, engines);
        CHECK_EQ(min_next_in_subcoordinators<float>(engines), 0.5f);
        // (2) collect outputs at t = 0.5, then (3) route thermostat -> heater (the IC)
        collect_outputs_in_subcoordinators<float>(0.5f, engines);
        route_internal_coupled_messages_on_subcoordinators<float, dynamic_log>(ics);
        CHECK(heater_sim->inbox().count(typeid(heater_defs::command)) == 1);
        CHECK(contains(memory_sink<routing_tag>::contents(), "heater ON (reading 15)"));
        // (4) collect what leaves the coupled model through its EOCs
        cadmium::dynamic::message_bags leaving = collect_messages_by_eoc<float, dynamic_log>(eocs);
        CHECK(leaving.empty());  // no alarm was raised: nothing to route out
        // (5) an outside input is routed to a sub-engine through an EIC
        external_couplings<float> eics;
        external_coupling<float> eic;
        eic.first = thermo_sim;
        eic.second.push_back(cadmium::dynamic::translate::make_link<thermostat_defs::reading, thermostat_defs::reading>());
        eics.push_back(eic);
        cadmium::dynamic::message_bags outside;
        outside[typeid(thermostat_defs::reading)] = cadmium::message_bag<thermostat_defs::reading>{22.0f};
        route_external_input_coupled_messages_on_subcoordinators<float, dynamic_log>(outside, eics);
        CHECK(thermo_sim->inbox().count(typeid(thermostat_defs::reading)) == 1);
        // (6) advance everybody: heater switches on, thermostat commits its decision
        advance_simulation_in_subengines<float>(0.5f, engines);
        CHECK(h_state_contains(heat, "ON"));
        // the type aliases that describe a coordinator's wiring
        static_assert(std::is_same<external_port_couplings, std::map<std::string, std::vector<std::shared_ptr<link_abstract>>>>::value,
                      "external_port_couplings maps port names to links");
        // messages_by_port_as_string: how the simulator formats an outbox for the log
        cadmium::dynamic::message_bags box;
        box[typeid(heater_defs::energy_pulse)] = cadmium::message_bag<heater_defs::energy_pulse>{1};
        CHECK_EQ(heat->messages_by_port_as_string(box), std::string("[greenhouse::heater_defs::energy_pulse: {1}]"));
    }

    // =====================================================================================
    section("PART 2c. links and link validation");
    // =====================================================================================
    {
        using cadmium::dynamic::engine::link;
        using cadmium::dynamic::engine::link_abstract;
        // a link remembers BOTH port types (static_assert: they must carry the same message type)
        link<heater_defs::energy_pulse, meter_defs::add> l;
        CHECK(l.from_port_type_index() == std::type_index(typeid(heater_defs::energy_pulse)));
        CHECK(l.to_port_type_index() == std::type_index(typeid(meter_defs::add)));
        CHECK(l.from_type_index() == std::type_index(typeid(cadmium::message_bag<heater_defs::energy_pulse>)));
        CHECK(l.to_type_index() == std::type_index(typeid(cadmium::message_bag<meter_defs::add>)));

        cadmium::dynamic::message_bags from, to;
        // nothing to route -> empty result, nothing created in the destination
        auto none = l.route_messages(from, to);
        CHECK(none.from_messages.empty() && none.to_messages.empty());
        CHECK(to.empty());
        // messages in the source bag, no destination bag yet: a NEW destination bag is created
        from[typeid(heater_defs::energy_pulse)] = cadmium::message_bag<heater_defs::energy_pulse>{1, 1};
        CHECK(l.is_there_messages_to_route(from));
        auto first = l.route_messages(from, to);
        CHECK_EQ(first.to_messages.size(), 2u);
        CHECK_EQ(first.from_port, std::string(boost::typeindex::type_id<heater_defs::energy_pulse>().pretty_name()));
        // destination bag already present: messages are APPENDED (several sources may feed one port)
        auto second = l.route_messages(from, to);
        CHECK_EQ(second.to_messages.size(), 4u);
        // the pieces route_messages is made of can be called directly
        cadmium::dynamic::message_bags fresh;
        auto direct = l.pass_messages_to_new_bag(from.at(typeid(heater_defs::energy_pulse)), fresh);
        CHECK_EQ(direct.to_messages.size(), 2u);
        auto appended = l.pass_messages(from.at(typeid(heater_defs::energy_pulse)), fresh.at(typeid(meter_defs::add)));
        CHECK_EQ(appended.to_messages.size(), 4u);

        // routed_messages: the record each routing step logs
        cadmium::dynamic::logger::routed_messages r("out", "in");
        CHECK(r.from_messages.empty());
        cadmium::dynamic::logger::routed_messages copy(first);
        CHECK(copy.to_messages == first.to_messages);
        cadmium::dynamic::logger::routed_messages blank;
        CHECK(blank.from_port.empty());

        // validation functions (what the coupled<> constructors call)
        auto a = cadmium::dynamic::translate::make_dynamic_atomic_model<heater, float>("a");
        auto b = cadmium::dynamic::translate::make_dynamic_atomic_model<energy_meter, float>("b");
        Models models{a, b};
        ICs good{cadmium::dynamic::translate::make_IC<heater_defs::energy_pulse, meter_defs::add>("a", "b")};
        ICs bad{cadmium::dynamic::translate::make_IC<heater_defs::energy_pulse, meter_defs::add>("b", "a")};
        CHECK(valid_ic_links(models, good));
        CHECK(!valid_ic_links(models, bad));  // wrong direction: the meter has no energy_pulse output
        Ports a_in = a->get_input_ports();
        EICs eic_ok{cadmium::dynamic::translate::make_EIC<heater_defs::command, heater_defs::command>("a")};
        CHECK(valid_eic_links(models, a_in, eic_ok));
        CHECK(!valid_eic_links(models, Ports{}, eic_ok));  // the coupled model declares no such input
        Ports meter_out = b->get_output_ports();
        EOCs eoc_ok{cadmium::dynamic::translate::make_EOC<meter_defs::sum, meter_defs::sum>("b")};
        CHECK(valid_eoc_links(models, meter_out, eoc_ok));
        CHECK(!valid_eoc_links(models, Ports{}, eoc_ok));
        // the plain data classes behind couplings
        cadmium::dynamic::modeling::EIC e("b", eic_ok[0]._link);
        cadmium::dynamic::modeling::EOC o("b", eoc_ok[0]._link);
        cadmium::dynamic::modeling::IC c("a", "b", good[0]._link);
        CHECK(cadmium::dynamic::modeling::EIC(e)._to == "b" && cadmium::dynamic::modeling::EOC(o)._from == "b" &&
              cadmium::dynamic::modeling::IC(c)._to == "b");
    }

    // =====================================================================================
    section("PART 2d. the translator's building blocks");
    // =====================================================================================
    {
        using namespace cadmium::dynamic::translate;
        // make_ports: a tuple of port types -> vector of type_index
        auto ports = make_ports<irrigation_model<float>::output_ports>();
        CHECK_EQ(ports.size(), 2u);
        // make_link: the type-erased link for a PORT_FROM/PORT_TO pair
        auto lnk = make_link<pulse_clock_defs::out, meter_defs::add>();
        CHECK(lnk->to_port_type_index() == std::type_index(typeid(meter_defs::add)));
        // make_dynamic_models: one dynamic atomic per sub-model of a static coupled model, by type
        using irrigation = irrigation_model<float>;
        using translator = make_dynamic_coupled_model_impl<float, irrigation_model>;
        models_by_type models = make_dynamic_models<float, irrigation::models, translator::coupled_model_translator>();
        CHECK_EQ(models.size(), 3u);
        // make_dynamic_ic / eic / eoc: static coupling tuples -> run-time vectors, using the ids above
        auto ics = make_dynamic_ic<float, irrigation::internal_couplings>(models);
        auto eocs = make_dynamic_eoc<float, irrigation::external_output_couplings>(models);
        auto eics = make_dynamic_eic<float, irrigation::external_input_couplings>(models);
        CHECK_EQ(ics.size(), 2u);
        CHECK_EQ(eocs.size(), 2u);
        CHECK_EQ(eics.size(), 0u);
        // a coupling that names a sub-model which was not translated is rejected
        models_by_type incomplete;
        CHECK_THROWS((make_dynamic_ic<float, irrigation::internal_couplings>(incomplete)), std::domain_error);
        // the stand-alone atomic factory, and the whole-model factory
        auto atom = make_dynamic_atomic_model_impl<heater, float>().make();
        CHECK_EQ(atom->time_advance(), inf);
        auto whole = make_dynamic_coupled_model<float, irrigation_model>();
        CHECK_EQ(whole->_models.size(), 3u);
        CHECK_EQ(whole->get_input_ports().size(), 0u);
        CHECK_EQ(whole->get_output_ports().size(), 2u);
        // atomic<...> default constructor names the model after its C++ type
        cadmium::dynamic::modeling::atomic<heater, float> typed_atom;
        CHECK_EQ(typed_atom.get_id(), std::string("greenhouse::heater<float>"));
        // EOC/EIC/IC factory helpers
        auto e1 = make_EOC<meter_defs::sum, irrigation_ports::water_report>("water_meter");
        auto e2 = make_EIC<climate_ports::report_tick_in, meter_defs::reset>("energy_meter");
        auto e3 = make_IC<pulse_clock_defs::out, meter_defs::add>("pulse_clock", "water_meter");
        CHECK(e1._from == "water_meter" && e2._to == "energy_meter" && e3._from == "pulse_clock");
    }

    return check_summary("04_engine_internals");
}
