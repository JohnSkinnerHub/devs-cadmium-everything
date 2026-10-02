/**
 * @file 02_static_greenhouse.cpp
 * @brief Simulating the greenhouse with Cadmium's STATIC (template) engine.
 *
 * WHAT THIS DEMO SHOWS
 *   1. Compile-time validation of a hierarchical coupled model (`verify_static_greenhouse`).
 *   2. `cadmium::engine::runner`: construct, `run_until`, resume, `run_until_passivate`.
 *   3. The same model running with three different TIME types (float, double, fixed_time)
 *      and producing identical event traces, "time representation is independent of the model".
 *   4. Loggers in action: a `multilogger` fan-out to three in-memory sinks, the default logger,
 *      and a user-level `logger_debug` channel written by the models themselves.
 *   5. Reading a trace back to ASSERT what the simulation did. Every number checked below can
 *      be derived by hand from `data/sensor_feed.txt`, see the table in README.md.
 *
 * HOW A STATIC SIMULATION IS STARTED (the whole recipe)
 *     using LOGGER = multilogger< logger<SOURCE, FORMATTER, SINK>, ... >;
 *     cadmium::engine::runner<TIME, TOP_MODEL_TEMPLATE, LOGGER> runner{initial_time};
 *     runner.run_until(end_time);
 */
#include <iostream>
#include <sstream>

#include "greenhouse/check.hpp"
#include "greenhouse/debug_log.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/systems/static_greenhouse.hpp"
#include "greenhouse/trace.hpp"

#include <cadmium/engine/pdevs_runner.hpp>

using namespace greenhouse;
namespace lg = cadmium::logger;

// -----------------------------------------------------------------------------------------
// One logger setup per TIME type. Each loggers' *source* decides what it reacts to; all three
// write into the same in-memory sink (distinguished by Tag) using our trace_formatter.
// -----------------------------------------------------------------------------------------
template <typename TIME, typename Tag>
using trace_logger = lg::multilogger<
    lg::logger<lg::logger_global_time, trace_formatter<TIME>, memory_sink<Tag>>,
    lg::logger<lg::logger_messages, trace_formatter<TIME>, memory_sink<Tag>>,
    lg::logger<lg::logger_state, trace_formatter<TIME>, memory_sink<Tag>>>;

struct float_tag {};
struct double_tag {};
struct fixed_tag {};

/// Runs the full greenhouse to `end` with TIME and returns the parsed trace.
template <typename TIME, typename Tag>
std::vector<trace_event> run_greenhouse(double end) {
    memory_sink<Tag>::clear();
    // 1. the runner: initial time, model, logger. The constructor builds the whole simulator
    //    tree (a coordinator per coupled model, a simulator per atomic model) and initialises it.
    cadmium::engine::runner<TIME, greenhouse_top, trace_logger<TIME, Tag>> runner{TIME(0.0)};
    // 2. run. Events strictly before `end` are processed; the return value is the time of the
    //    next scheduled event (>= end).
    runner.run_until(TIME(end));
    return parse_trace(memory_sink<Tag>::contents());
}

// ---- the "same type twice" pitfall (section 7) --------------------------------------------
// A static coupled model identifies its sub-models by C++ type. `alias_meter` is merely another NAME
// for the same type as `plain_meter`; `derived_meter` is a genuinely different type.
template <typename T> using plain_meter = stock::accumulator<int, T>;
template <typename T> using alias_meter = stock::accumulator<int, T>;
template <typename T> struct derived_meter : public stock::accumulator<int, T> {};

template <template <typename> class SECOND_METER>
struct twin_meters {
    template <typename T>
    using type = coupled_model<
        T, no_ports, no_ports, models_tuple<pulse_clock, report_clock, plain_meter, SECOND_METER>, no_couplings,
        no_couplings,
        std::tuple<IC<pulse_clock, pulse_clock_defs::out, plain_meter, meter_defs::add>,
                   IC<pulse_clock, pulse_clock_defs::out, SECOND_METER, meter_defs::add>,
                   IC<report_clock, report_clock_defs::out, plain_meter, meter_defs::reset>,
                   IC<report_clock, report_clock_defs::out, SECOND_METER, meter_defs::reset>>>;
};
struct twin_tag {};

int main() {
    // --- 1. compile-time validation -----------------------------------------------------
    section("1. compile-time validation of the model hierarchy");
    verify_static_greenhouse();  // static_asserts fire at compile time; reaching here means they passed
    CHECK_MSG(true, "greenhouse_top passed every Cadmium concept check (ports, couplings, DEVS functions)");

    // --- 2. one run, with the debug channel switched on ---------------------------------
    section("2. run the greenhouse for 21 simulated seconds (float time)");
    std::ostringstream debug_text;  // models write here through greenhouse::debug_note()
    set_debug_target(&debug_text);
    auto trace = run_greenhouse<float, float_tag>(21.0);
    set_debug_target(nullptr);

    const auto water = reports_of(trace, "water_meter");
    const auto energy = reports_of(trace, "energy_meter");
    const auto alarm = reports_of(trace, "filter_first_output");

    std::cout << "  water  reports (time, units): ";
    for (auto& r : water) std::cout << "(" << r.first << "," << r.second << ") ";
    std::cout << "\n  energy reports (time, units): ";
    for (auto& r : energy) std::cout << "(" << r.first << "," << r.second << ") ";
    std::cout << "\n  alarm latch output (time, code): ";
    for (auto& r : alarm) std::cout << "(" << r.first << "," << r.second << ") ";
    std::cout << std::endl;

    // Expected values derived by hand (see README "What the simulation does"):
    //   water: one pulse per second, reported & reset every 5 s  -> 5 each time
    //   energy: heater pulses at 3.5 4.5 | 14.5 | 15.5 16.5 17.5 -> 2, 0, 1, 3 at t = 5,10,15,20
    //   alarm: first overheat decision at 7.5; the second one (12.5) is swallowed by the latch
    CHECK_EQ(water.size(), 4u);
    for (const auto& r : water) CHECK_EQ(r.second, 5);
    CHECK_EQ(energy.size(), 4u);
    CHECK(energy[0].first == 5 && energy[0].second == 2);
    CHECK(energy[1].first == 10 && energy[1].second == 0);
    CHECK(energy[2].first == 15 && energy[2].second == 1);
    CHECK(energy[3].first == 20 && energy[3].second == 3);
    CHECK_EQ(alarm.size(), 1u);
    CHECK(alarm[0].first == 7.5 && alarm[0].second == 1);

    // the thermostat raised the alarm twice (7.5 and 12.5); the latch let only the first through
    int thermostat_alarms = 0;
    for (auto& e : outputs_of(trace, "thermostat"))
        if (e.second.find("overheat_alarm: {1}") != std::string::npos) ++thermostat_alarms;
    CHECK_EQ(thermostat_alarms, 2);

    // Confluent events (simultaneous internal + external) counted through the debug channel:
    // 1 in the thermostat (reading at 2.5) and 2 in the heater (OFF command at 4.5 and 17.5).
    auto count = [](const std::string& hay, const std::string& needle) {
        int n = 0;
        for (auto p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
        return n;
    };
    CHECK_EQ(count(debug_text.str(), "thermostat: CONFLUENCE"), 1);
    CHECK_EQ(count(debug_text.str(), "heater: CONFLUENCE"), 2);

    // final states, straight from the state log
    CHECK_EQ(last_state_of(trace, "heater"), std::string("OFF sigma=inf pulses=6"));
    // alarm_panel is an *alias* of the stock passive<int,TIME>, so its logged id is "passive"
    CHECK_EQ(last_state_of(trace, "passive"), std::string("0"));

    // --- 3. time independence -----------------------------------------------------------
    section("3. same model, three TIME types, identical traces");
    auto trace_double = run_greenhouse<double, double_tag>(21.0);
    auto trace_fixed = run_greenhouse<fixed_time, fixed_tag>(21.0);
    auto same = [](const std::vector<trace_event>& a, const std::vector<trace_event>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].time != b[i].time || a[i].kind != b[i].kind || a[i].model != b[i].model || a[i].text != b[i].text)
                return false;
        return true;
    };
    CHECK_EQ(trace.size(), trace_double.size());
    CHECK(same(trace, trace_double));
    CHECK(same(trace, trace_fixed));
    std::cout << "  (" << trace.size() << " trace events compared for each representation)\n";

    // --- 4. incremental running ---------------------------------------------------------
    section("4. run_until can be called repeatedly; it resumes where it stopped");
    memory_sink<float_tag>::clear();
    {
        cadmium::engine::runner<float, greenhouse_top, trace_logger<float, float_tag>> runner{0.0f};
        float next = runner.run_until(5.0f);  // events at exactly 5.0 are NOT processed yet
        CHECK_EQ(next, 5.0f);
        auto first_part = parse_trace(memory_sink<float_tag>::contents());
        CHECK_EQ(reports_of(first_part, "water_meter").size(), 0u);  // the t=5 report is still pending
        next = runner.run_until(5.5f);                               // now it is processed
        CHECK_EQ(next, 6.0f);  // returns the time of the next UNprocessed event (the pulse at 6), not 5.5
        auto second_part = parse_trace(memory_sink<float_tag>::contents());
        CHECK_EQ(reports_of(second_part, "water_meter").size(), 1u);
    }

    // --- 5. run_until_passivate ---------------------------------------------------------
    section("5. run_until_passivate on a model that eventually has nothing left to do");
    memory_sink<float_tag>::clear();
    {
        // climate_standalone = the climate subsystem on its own: no clocks, so once the sensor
        // file is exhausted and the heater is off, every model is passive (ta = infinity).
        cadmium::engine::runner<float, climate_standalone, trace_logger<float, float_tag>> runner{0.0f};
        runner.run_until_passivate();  // returns when the next event time is infinity
        auto t = parse_trace(memory_sink<float_tag>::contents());
        // no reset ever reaches the energy meter here, so it just kept counting: 6 pulses
        CHECK_EQ(last_state_of(t, "energy_meter"), std::string("[6, 0]"));
        double last_time = 0;
        for (auto& e : t) last_time = e.time;
        CHECK_EQ(last_time, 17.5);  // the last thing that happened: the OFF decision at 17.5
        std::cout << "  last event processed at t=" << last_time << "\n";
    }

    // --- 6. the default logger ----------------------------------------------------------
    section("6. the default logger: cadmium::engine::default_logger prints the *state* of each model to cout");
    {
        std::ostringstream captured;
        auto* old = std::cout.rdbuf(captured.rdbuf());  // capture std::cout, the default sink
        {
            cadmium::engine::runner<float, climate_standalone> runner{0.0f};  // no LOGGER argument given
            runner.run_until(0.1f);
        }
        std::cout.rdbuf(old);
        std::cout << "  first captured line: " << captured.str().substr(0, captured.str().find('\n')) << "\n";
        CHECK(captured.str().find("State for model") != std::string::npos);
    }

    // --- 7. a silent pitfall ------------------------------------------------------------
    section("7. pitfall: a static coupled model cannot hold the same type twice");
    {
        // Two meters fed by the same clocks should each report 5 at t = 5.
        memory_sink<twin_tag>::clear();
        {
            cadmium::engine::runner<float, twin_meters<derived_meter>::type, trace_logger<float, twin_tag>> runner{0.0f};
            runner.run_until(6.0f);
        }
        auto distinct = parse_trace(memory_sink<twin_tag>::contents());
        CHECK_EQ(reports_of(distinct, "accumulator").size(), 1u);
        CHECK_EQ(reports_of(distinct, "derived_meter").size(), 1u);
        CHECK_EQ(reports_of(distinct, "accumulator")[0].second, 5);
        CHECK_EQ(reports_of(distinct, "derived_meter")[0].second, 5);

        // The same model with an alias (= the same type again) COMPILES, runs, and is silently wrong:
        // both sets of couplings are resolved to ONE engine, which receives every message twice
        // (reports 10), while the second sub-model never takes part. Nothing diagnoses it.
        memory_sink<twin_tag>::clear();
        {
            cadmium::engine::runner<float, twin_meters<alias_meter>::type, trace_logger<float, twin_tag>> runner{0.0f};
            runner.run_until(6.0f);
        }
        auto collapsed = parse_trace(memory_sink<twin_tag>::contents());
        auto sums = reports_of(collapsed, "accumulator");
        std::cout << "  same type twice -> reports: ";
        for (auto& r : sums) std::cout << "(" << r.first << "," << r.second << ") ";
        std::cout << "(two meters should have reported 5 each)\n";
        CHECK_EQ(sums.size(), 1u);
        CHECK_EQ(sums[0].second, 10);
        // (if these two checks ever fail, Cadmium has started diagnosing or handling the duplicate)
    }

    return check_summary("02_static_greenhouse");
}
