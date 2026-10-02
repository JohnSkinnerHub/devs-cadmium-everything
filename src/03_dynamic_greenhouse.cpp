/**
 * @file 03_dynamic_greenhouse.cpp
 * @brief Simulating the greenhouse with Cadmium's DYNAMIC engine, in three execution modes.
 *
 * WHAT THIS DEMO SHOWS
 *   1. Translating the *static* model of demo 02 into a dynamic one and checking that the
 *      dynamic engine produces the same simulation as the static engine.
 *   2. Building a dynamic model by hand: model ids, constructor arguments, nested coupled
 *      models, vector and initializer-list constructors. Same classes, different parameters,
 *      different (and predictable) results.
 *   3. Run-time validation: wrong couplings are rejected with `std::domain_error`.
 *   4. The dynamic runner's extras: `turn_progress_on/off`, `run_until_passivate`.
 *   5. Execution modes, selected when COMPILING this very file (see the Makefile):
 *        (default)                 single thread
 *        -DCADMIUM_EXECUTE_CONCURRENT  Boost.Thread pool: sub-models advance concurrently
 *        -DCPU_PARALLEL -fopenmp       OpenMP: same idea, different runtime
 *      The simulation must give the same answer in every mode, the models are independent
 *      within one simulation step, so concurrency changes the speed, not the result.
 *
 * Run with an argument to dump the canonical trace (used by `make test` to diff the 3 modes):
 *      build/03_dynamic_greenhouse --dump build/out/trace_sequential.txt
 */
#include <algorithm>
#include <atomic>
#include <fstream>
#include <numeric>
#include <iostream>
#include <map>
#include <sstream>
#include <tuple>

#include "greenhouse/check.hpp"
#include "greenhouse/debug_log.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/systems/dynamic_greenhouse.hpp"
#include "greenhouse/trace.hpp"

#include <cadmium/engine/pdevs_dynamic_runner.hpp>
#include <cadmium/engine/pdevs_runner.hpp>  // the static runner, for the cross-check in section 1

using namespace greenhouse;
namespace lg = cadmium::logger;

#if defined(CADMIUM_EXECUTE_CONCURRENT)
static const char* const kMode = "concurrent (Boost.Thread pool)";
#elif defined(CPU_PARALLEL)
static const char* const kMode = "parallel (OpenMP)";
#else
static const char* const kMode = "sequential";
#endif

template <typename TIME, typename Tag>
using trace_logger = lg::multilogger<
    lg::logger<lg::logger_global_time, trace_formatter<TIME>, memory_sink<Tag>>,
    lg::logger<lg::logger_messages, trace_formatter<TIME>, memory_sink<Tag>>,
    lg::logger<lg::logger_state, trace_formatter<TIME>, memory_sink<Tag>>>;

struct static_tag {};
struct translated_tag {};
struct handmade_tag {};
struct tuned_tag {};
struct stress_tag {};
struct passive_tag {};

/// The dynamic runner's constructor differs by mode: the parallel modes take a thread count.
template <typename TIME, typename LOGGER>
cadmium::dynamic::engine::runner<TIME, LOGGER> make_runner(
        const std::shared_ptr<cadmium::dynamic::modeling::coupled<TIME>>& top) {
#if defined(CADMIUM_EXECUTE_CONCURRENT) || defined(CPU_PARALLEL)
    return cadmium::dynamic::engine::runner<TIME, LOGGER>(top, TIME(0.0), 4);  // 4 worker threads
#else
    return cadmium::dynamic::engine::runner<TIME, LOGGER>(top, TIME(0.0));
#endif
}

/// Runs a dynamic model for `end` seconds and returns its parsed trace.
template <typename TIME, typename Tag>
std::vector<trace_event> run_dynamic(const std::shared_ptr<cadmium::dynamic::modeling::coupled<TIME>>& top, double end) {
    memory_sink<Tag>::clear();
    auto runner = make_runner<TIME, trace_logger<TIME, Tag>>(top);
    runner.run_until(TIME(end));
    return parse_trace(memory_sink<Tag>::contents());
}

/// Only what matters for comparing two engines: non-empty outputs and states, in a canonical order.
/// (Within one simulation instant the models' log lines may come in any order, that order is
/// not part of the simulation semantics, and with threads it is not even deterministic.)
using canon_t = std::vector<std::tuple<double, char, std::string, std::string>>;
static canon_t canonical(const std::vector<trace_event>& trace, const std::map<std::string, std::string>& rename = {}) {
    canon_t c;
    for (const auto& e : trace) {
        if (e.kind == 'O' && !has_messages(e.text)) continue;
        auto it = rename.find(e.model);
        c.emplace_back(e.time, e.kind, it == rename.end() ? e.model : it->second, e.text);
    }
    std::sort(c.begin(), c.end());
    return c;
}

int main(int argc, char** argv) {
#if defined(CPU_PARALLEL) && !defined(CADMIUM_EXECUTE_CONCURRENT)
    // GOTCHA (found while writing this demo): Cadmium's OpenMP mode opens an `omp parallel`
    // region for the sub-models of EVERY coupled model, so a coupled model nested inside
    // another one opens a *nested* region. OpenMP serialises nested regions by default (team of
    // one thread), and `cpu_parallel_for_each` hands "the remainder" of the work to the last
    // thread of the team it asked for, which then never exists: the remaining sub-models are
    // silently never advanced, simulated time stops moving and the program hangs.
    // Allowing nested parallel regions fixes it (same effect as OMP_MAX_ACTIVE_LEVELS=16).
    omp_set_max_active_levels(16);
#endif
    std::cout << "Dynamic engine demo, execution mode: " << kMode << std::endl;
    const std::string feed = "data/sensor_feed.txt";

    // ---------------------------------------------------------------------------------
    section("1. translate the static model -> dynamic, and compare with the static engine");
    // ---------------------------------------------------------------------------------
    // The static engine's trace (float) ...
    memory_sink<static_tag>::clear();
    {
        cadmium::engine::runner<float, greenhouse_top, trace_logger<float, static_tag>> runner{0.0f};
        runner.run_until(21.0f);
    }
    auto static_trace = parse_trace(memory_sink<static_tag>::contents());
    // ... and the dynamic engine running the *translated* model (same ids: C++ type names).
    auto translated = translate_static_greenhouse<float>();
    auto translated_trace = run_dynamic<float, translated_tag>(translated.top, 21.0);

    std::cout << "  dynamic atomic models found by translation:";
    for (auto& a : translated.atoms) std::cout << " " << short_model_name(a.first);
    std::cout << "\n";
    CHECK_EQ(translated.atoms.size(), 9u);
    CHECK(canonical(static_trace) == canonical(translated_trace));
    std::cout << "  (" << canonical(static_trace).size() << " events: static and dynamic engines agree)\n";

    // ---------------------------------------------------------------------------------
    section("2. a hand-built dynamic model: ids, constructor arguments, nested coupled models");
    // ---------------------------------------------------------------------------------
    auto handmade = build_dynamic_greenhouse<float>();  // default parameters
    auto handmade_trace = run_dynamic<float, handmade_tag>(handmade.top, 21.0);
    // The hand-built model uses readable ids; map the stock models' type names onto them.
    const std::map<std::string, std::string> rename{{"int_generator_one_sec", ids::pulse_clock},
                                                    {"reset_generator_five_sec", ids::report_clock},
                                                    {"filter_first_output", ids::alarm_latch},
                                                    {"passive", ids::alarm_panel}};
    CHECK(canonical(static_trace, rename) == canonical(handmade_trace));
    std::cout << "  with default parameters it is again the same simulation as the static one\n";

    // Same classes, different constructor arguments: a heater that pulses every 0.5 s.
    greenhouse_options tuned;
    tuned.heater.pulse_period_s = 0.5;
    auto tuned_model = build_dynamic_greenhouse<float>(tuned);
    auto tuned_trace = run_dynamic<float, tuned_tag>(tuned_model.top, 21.0);
    auto energy = reports_of(tuned_trace, ids::energy_meter);
    std::cout << "  energy reports with pulse_period_s=0.5 (time, units): ";
    for (auto& r : energy) std::cout << "(" << r.first << "," << r.second << ") ";
    std::cout << "\n";
    // Derived by hand: ON during [2.5, 4.5] -> pulses 3.0 3.5 4.0 4.5 = 4; ON from 13.5 ->
    // 14.0 14.5 15.0 (the t=15 pulse and the t=15 reset arrive together, so it counts) = 3;
    // then 15.5 16.0 16.5 17.0 17.5 = 5.
    CHECK_EQ(energy.size(), 4u);
    CHECK(energy[0].second == 4 && energy[1].second == 0 && energy[2].second == 3 && energy[3].second == 5);
    // ... and the water meter does not care:
    for (auto& r : reports_of(tuned_trace, ids::water_meter)) CHECK_EQ(r.second, 5);

    // The atomic models are reachable, so their final state can be read without parsing logs.
    CHECK_EQ(handmade.atoms.at(ids::heater)->model_state_as_string(), std::string("OFF sigma=inf pulses=6"));
    CHECK_EQ(tuned_model.atoms.at(ids::heater)->model_state_as_string(), std::string("OFF sigma=inf pulses=12"));
    std::cout << "  thermostat final state: " << handmade.atoms.at(ids::thermostat)->model_state_as_string() << "\n";

    // ---------------------------------------------------------------------------------
    section("3. run-time validation of a dynamic model");
    // ---------------------------------------------------------------------------------
    {
        using namespace cadmium::dynamic::modeling;
        using namespace cadmium::dynamic::translate;
        auto a = make_dynamic_atomic_model<heater, float>("a_heater");
        auto b = make_dynamic_atomic_model<energy_meter, float>("a_meter");
        Models two{a, b};
        Ports none;
        // valid: heater.energy_pulse -> meter.add
        ICs good{make_IC<heater_defs::energy_pulse, meter_defs::add>("a_heater", "a_meter")};
        coupled<float> ok("ok", two, none, none, EICs{}, EOCs{}, good);
        CHECK_EQ(ok.get_id(), std::string("ok"));
        // invalid: a model id that does not exist
        ICs ghost{make_IC<heater_defs::energy_pulse, meter_defs::add>("a_heater", "no_such_model")};
        CHECK_THROWS(coupled<float>("bad1", two, none, none, EICs{}, EOCs{}, ghost), std::domain_error);
        // invalid: the 'from' port is not an output port of that model (the meter's `add` is an input)
        ICs wrong_port{make_IC<meter_defs::add, meter_defs::add>("a_meter", "a_meter")};
        CHECK_THROWS(coupled<float>("bad2", two, none, none, EICs{}, EOCs{}, wrong_port), std::domain_error);
        // invalid EIC: the coupled model does not declare the external input port it is fed from
        EICs bad_eic{make_EIC<heater_defs::command, heater_defs::command>("a_heater")};
        CHECK_THROWS(coupled<float>("bad3", two, none, none, bad_eic, EOCs{}, ICs{}), std::domain_error);
        // invalid EOC: the sub-model has no such output port
        EOCs bad_eoc{make_EOC<meter_defs::sum, meter_defs::sum>("a_heater")};
        Ports out_sum{typeid(meter_defs::sum)};
        CHECK_THROWS(coupled<float>("bad4", two, none, out_sum, EICs{}, bad_eoc, ICs{}), std::domain_error);
        std::cout << "  (mismatched MESSAGE TYPES are caught earlier still: at compile time, see tests/compile_fail)\n";
    }

    // ---------------------------------------------------------------------------------
    section("4. dynamic runner extras: progress meter and run_until_passivate");
    // ---------------------------------------------------------------------------------
    {
        std::ostringstream captured;
        auto* old = std::cout.rdbuf(captured.rdbuf());
        {
            auto g = build_dynamic_greenhouse<float>();
            using quiet = lg::not_logger;  // a logger that never matches any source = logging off
            cadmium::dynamic::engine::runner<float, quiet> runner(g.top, 0.0f);
            runner.turn_progress_on();   // prints "\r[<time>/<horizon>]" after every step, in yellow
            runner.run_until(8.0f);
            runner.turn_progress_off();  // (run_until also switches it off when it finishes)
        }
        std::cout.rdbuf(old);
        const std::string out = captured.str();
        CHECK(out.find("/8]") != std::string::npos);
        CHECK(out.find("[7.5/8]") != std::string::npos);
        std::cout << "  progress output ends with: ..." << out.substr(out.rfind("[7")) << "\n";

        // progress_bar_meter(current, total) is the public function the runner calls; "inf" for run_until_passivate
        std::ostringstream meter_out;
        auto* old2 = std::cout.rdbuf(meter_out.rdbuf());
        {
            auto g = build_dynamic_greenhouse<float>();
            cadmium::dynamic::engine::runner<float, lg::not_logger> runner(g.top, 0.0f);
            runner.progress_bar_meter(2.5f, 10.0f);
            runner.progress_bar_meter(2.5f, std::numeric_limits<float>::infinity());
        }
        std::cout.rdbuf(old2);
        CHECK(meter_out.str().find("[2.5/10]") != std::string::npos);
        CHECK(meter_out.str().find("[2.5/inf]") != std::string::npos);
    }
    memory_sink<passive_tag>::clear();
    {
        // The climate subsystem alone becomes passive after the sensor file ends.
        std::map<std::string, std::shared_ptr<cadmium::dynamic::modeling::atomic_abstract<float>>> atoms;
        auto climate_only = build_dynamic_climate<float>(greenhouse_options{}, atoms);
        auto runner = make_runner<float, trace_logger<float, passive_tag>>(climate_only);
        runner.run_until_passivate();
        CHECK_EQ(atoms.at(ids::energy_meter)->model_state_as_string(), std::string("[6, 0]"));
        CHECK_EQ(atoms.at(ids::energy_meter)->time_advance(), std::numeric_limits<float>::infinity());
    }

    // ---------------------------------------------------------------------------------
    section("5. other TIME types on the dynamic engine");
    // ---------------------------------------------------------------------------------
    {
        auto g = build_dynamic_greenhouse<fixed_time>();
        auto t = run_dynamic<fixed_time, handmade_tag>(g.top, 21.0);
        CHECK(canonical(t) == canonical(handmade_trace));
        std::cout << "  fixed_time gives the same events as float\n";
    }

    // ---------------------------------------------------------------------------------
    section(std::string("6. determinism in ") + kMode + " mode");
    // ---------------------------------------------------------------------------------
    // The simulation outcome must not depend on the execution mode. 25 repetitions give
    // a data race (if there were one) plenty of chances to show.
    {
        const canon_t reference = canonical(static_trace, rename);
        int identical = 0;
        const int repetitions = 25;
        for (int i = 0; i < repetitions; ++i) {
            auto g = build_dynamic_greenhouse<float>();
            auto t = run_dynamic<float, stress_tag>(g.top, 21.0);
            if (canonical(t) == reference) ++identical;
        }
        CHECK_EQ(identical, repetitions);
        std::cout << "  " << identical << "/" << repetitions << " runs identical to the static reference\n";
    }

    // ---------------------------------------------------------------------------------
    section("7. the parallel building blocks, called directly");
    // ---------------------------------------------------------------------------------
#if defined(CADMIUM_EXECUTE_CONCURRENT)
    {
        // concurrent_for_each(pool, first, last, f): runs f(*it) for every element on the pool and
        // returns when all are done. (The element is bound by value, so f records into shared state.)
        boost::basic_thread_pool pool(4);
        std::vector<int> numbers(100);
        std::iota(numbers.begin(), numbers.end(), 1);
        std::atomic<int> sum{0}, calls{0};
        auto add = [&sum, &calls](int n) { sum += n; ++calls; };
        cadmium::concurrency::concurrent_for_each(pool, numbers.begin(), numbers.end(), add);
        CHECK_EQ(sum.load(), 5050);
        CHECK_EQ(calls.load(), 100);  // every element exactly once
    }
#elif defined(CPU_PARALLEL)
    {
        // cpu_parallel_for_each(first, last, f, threads): the OpenMP equivalent
        std::vector<int> numbers(100);
        std::iota(numbers.begin(), numbers.end(), 1);
        std::atomic<int> sum{0}, calls{0};
        auto add = [&sum, &calls](int n) { sum += n; ++calls; };
        cadmium::parallel::cpu_parallel_for_each(numbers.begin(), numbers.end(), add, 4);
        CHECK_EQ(sum.load(), 5050);
        CHECK_EQ(calls.load(), 100);
        // fewer elements than threads: the thread count is reduced to the element count
        std::vector<int> few{1, 2};
        sum = 0;
        calls = 0;
        cadmium::parallel::cpu_parallel_for_each(few.begin(), few.end(), add, 8);
        CHECK_EQ(sum.load(), 3);
        CHECK_EQ(calls.load(), 2);
    }
#else
    CHECK_MSG(true, "sequential build: concurrent_for_each / cpu_parallel_for_each are exercised in the other two builds");
#endif

    // optional: dump the canonical trace so `make test` can diff the three modes byte for byte
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--dump") {
            std::ofstream out(argv[i + 1]);
            for (const auto& e : canonical(handmade_trace))
                out << std::get<0>(e) << '\t' << std::get<1>(e) << '\t' << std::get<2>(e) << '\t' << std::get<3>(e) << '\n';
            std::cout << "  canonical trace written to " << argv[i + 1] << "\n";
        }
    }
    return check_summary("03_dynamic_greenhouse");
}
