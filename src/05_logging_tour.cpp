/**
 * @file 05_logging_tour.cpp
 * @brief A complete tour of Cadmium's logging system.
 *
 * Cadmium's loggers are the ONLY way to observe a running simulation (the runner returns no
 * results), so everything about them matters. Recap of the design (see also logging.hpp):
 *
 *     logger<SOURCE, FORMATTER, SINK_PROVIDER>::log<SOURCE, EVENT>(args...)
 *
 *   SOURCE  = the *category* a user filters on (7 of them)
 *   EVENT   = the *moment* in the algorithm that produced the line (18 of them)
 *   FORMATTER = static functions, one per event, that build the text
 *   SINK_PROVIDER = `static std::ostream& sink()`
 *
 * WHAT THIS DEMO SHOWS
 *   1. The complete source/event matrix: one logger per source, one sink per source, a run on
 *      each engine, and a check that every event produced its characteristic line.
 *   2. Custom sinks (file, in-memory, cerr/cout redirection) and a custom formatter.
 *   3. The null logger (`not_logger`) and `verbatim_formatter`.
 *   4. The message-printing helpers: is_streamable, value_or_name, implode, messages_as_strings,
 *      print_messages_by_port.
 *   5. The default loggers of both runners.
 *   6. The user-level `logger_debug` channel.
 *   7. Thread safety of our sinks.
 */
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "greenhouse/check.hpp"
#include "greenhouse/debug_log.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/systems/dynamic_greenhouse.hpp"
#include "greenhouse/systems/static_greenhouse.hpp"
#include "greenhouse/trace.hpp"

#include <cadmium/engine/pdevs_dynamic_runner.hpp>
#include <cadmium/engine/pdevs_runner.hpp>
#include <cadmium/logger/common_loggers_helpers.hpp>
#include <cadmium/logger/tuple_to_ostream.hpp>

using namespace greenhouse;
namespace lg = cadmium::logger;

// One sink per SOURCE, so we can see which source produced which text.
struct tag_info {};
struct tag_state {};
struct tag_messages {};
struct tag_routing {};
struct tag_global {};
struct tag_local {};
struct tag_debug {};

template <typename Formatter>
using all_sources = lg::multilogger<
    lg::logger<lg::logger_info, Formatter, memory_sink<tag_info>>,
    lg::logger<lg::logger_state, Formatter, memory_sink<tag_state>>,
    lg::logger<lg::logger_messages, Formatter, memory_sink<tag_messages>>,
    lg::logger<lg::logger_message_routing, Formatter, memory_sink<tag_routing>>,
    lg::logger<lg::logger_global_time, Formatter, memory_sink<tag_global>>,
    lg::logger<lg::logger_local_time, Formatter, memory_sink<tag_local>>,
    lg::logger<lg::logger_debug, Formatter, memory_sink<tag_debug>>>;

static void clear_all() {
    memory_sink<tag_info>::clear();
    memory_sink<tag_state>::clear();
    memory_sink<tag_messages>::clear();
    memory_sink<tag_routing>::clear();
    memory_sink<tag_global>::clear();
    memory_sink<tag_local>::clear();
    memory_sink<tag_debug>::clear();
}
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

/// One row of the source/event matrix.
struct event_row {
    const char* event;       // Cadmium's event type
    const char* source;      // the source it is emitted under
    std::string (*sink)();   // where to look
    const char* needle;      // text its stock formatter produces
};
static std::string read_info() { return memory_sink<tag_info>::contents(); }
static std::string read_state() { return memory_sink<tag_state>::contents(); }
static std::string read_messages() { return memory_sink<tag_messages>::contents(); }
static std::string read_routing() { return memory_sink<tag_routing>::contents(); }
static std::string read_global() { return memory_sink<tag_global>::contents(); }
static std::string read_local() { return memory_sink<tag_local>::contents(); }

struct unprintable {};  // a message type with no operator<<

int main() {
    // =====================================================================================
    section("1a. the source/event matrix on the STATIC engine");
    // =====================================================================================
    clear_all();
    {
        cadmium::engine::runner<float, greenhouse_top, all_sources<lg::formatter<float>>> runner{0.0f};
        runner.run_until(6.0f);
    }
    const std::vector<event_row> static_rows = {
        {"run_info", "logger_info", read_info, "Preparing model"},
        {"run_info", "logger_info", read_info, "Starting run"},
        {"run_info", "logger_info", read_info, "Finished run"},
        {"coor_info_init", "logger_info", read_info, "initialized to time 0"},
        {"coor_info_collect", "logger_info", read_info, "collecting output at time"},
        {"coor_info_advance", "logger_info", read_info, "advancing simulation from time"},
        {"sim_info_init", "logger_info", read_info, "Simulator for model"},
        {"sim_info_collect", "logger_info", read_info, "Simulator for model greenhouse::heater<float> collecting output"},
        {"sim_info_advance", "logger_info", read_info, "Simulator for model greenhouse::heater<float> advancing simulation"},
        {"sim_state", "logger_state", read_state, "State for model greenhouse::heater<float> is OFF sigma=inf pulses=0"},
        {"sim_messages_collect", "logger_messages", read_messages, "generated by model greenhouse::heater<float>"},
        {"coor_routing_eoc_collect", "logger_message_routing", read_routing, "EOC for model"},
        {"coor_routing_ic_collect", "logger_message_routing", read_routing, "IC for model"},
        {"coor_routing_eic_collect", "logger_message_routing", read_routing, "EIC for model"},
        {"coor_routing_collect_ic", "logger_message_routing", read_routing, "routed from greenhouse::thermostat_defs::heater_command_out of model"},
        {"coor_routing_collect_eic", "logger_message_routing", read_routing, "routed from greenhouse::climate_ports::report_tick_in with messages"},
        {"coor_routing_collect_eoc", "logger_message_routing", read_routing, "of model greenhouse::energy_meter<float> with messages"},
        {"sim_local_time", "logger_local_time", read_local, "Elapsed in model greenhouse::heater<float> is"},
        {"run_global_time", "logger_global_time", read_global, "5\n"},
    };
    for (const auto& row : static_rows)
        CHECK_MSG(has(row.sink(), row.needle), std::string(row.event) + "  (" + row.source + ")  -> \"" + row.needle + "\"");
    CHECK_MSG(memory_sink<tag_debug>::contents().empty(), "logger_debug: no engine ever emits on it (reserved for model authors)");
    // every source derives from logger_source, every event from logger_event (the two base tags)
    static_assert(std::is_base_of<lg::logger_source, lg::logger_info>::value && std::is_base_of<lg::logger_source, lg::logger_debug>::value &&
                  std::is_base_of<lg::logger_source, lg::logger_state>::value && std::is_base_of<lg::logger_source, lg::logger_messages>::value &&
                  std::is_base_of<lg::logger_source, lg::logger_message_routing>::value &&
                  std::is_base_of<lg::logger_source, lg::logger_global_time>::value && std::is_base_of<lg::logger_source, lg::logger_local_time>::value,
                  "the 7 sources");
    static_assert(std::is_base_of<lg::logger_event, lg::coor_info_init>::value && std::is_base_of<lg::logger_event, lg::coor_info_collect>::value &&
                  std::is_base_of<lg::logger_event, lg::coor_routing_collect>::value && std::is_base_of<lg::logger_event, lg::coor_routing_collect_ic>::value &&
                  std::is_base_of<lg::logger_event, lg::coor_routing_collect_eic>::value && std::is_base_of<lg::logger_event, lg::coor_routing_collect_eoc>::value &&
                  std::is_base_of<lg::logger_event, lg::coor_info_advance>::value && std::is_base_of<lg::logger_event, lg::coor_routing_ic_collect>::value &&
                  std::is_base_of<lg::logger_event, lg::coor_routing_eic_collect>::value && std::is_base_of<lg::logger_event, lg::coor_routing_eoc_collect>::value &&
                  std::is_base_of<lg::logger_event, lg::sim_info_init>::value && std::is_base_of<lg::logger_event, lg::sim_state>::value &&
                  std::is_base_of<lg::logger_event, lg::sim_info_collect>::value && std::is_base_of<lg::logger_event, lg::sim_messages_collect>::value &&
                  std::is_base_of<lg::logger_event, lg::sim_info_advance>::value && std::is_base_of<lg::logger_event, lg::sim_local_time>::value &&
                  std::is_base_of<lg::logger_event, lg::run_global_time>::value && std::is_base_of<lg::logger_event, lg::run_info>::value,
                  "the 18 events");
    CHECK_MSG(true, "7 sources derive from logger_source and 18 events from logger_event (static_assert)");
    std::cout << "  sample of each source:\n";
    auto first_line = [](const std::string& s) { return s.substr(0, s.find('\n')); };
    std::cout << "    info    : " << first_line(read_info()) << "\n";
    std::cout << "    state   : " << first_line(read_state()) << "\n";
    std::cout << "    messages: " << first_line(read_messages()) << "\n";
    std::cout << "    global  : " << first_line(read_global()) << "\n";
    std::cout << "    local   : " << first_line(read_local()) << "\n";

    // =====================================================================================
    section("1b. the same on the DYNAMIC engine (its formatter has different argument lists)");
    // =====================================================================================
    clear_all();
    {
        auto g = translate_static_greenhouse<float>();
        cadmium::dynamic::engine::runner<float, all_sources<cadmium::dynamic::logger::formatter<float>>> runner(g.top, 0.0f);
        runner.run_until(6.0f);
    }
    const std::vector<event_row> dynamic_rows = {
        {"run_info", "logger_info", read_info, "Starting run"},
        {"coor_info_init", "logger_info", read_info, "Coordinator for model"},
        {"sim_info_init", "logger_info", read_info, "Simulator for model"},
        {"sim_state", "logger_state", read_state, "State for model greenhouse::heater<float> is OFF sigma=inf pulses=0"},
        {"sim_messages_collect", "logger_messages", read_messages, "generated by model greenhouse::heater<float>"},
        {"coor_routing_eoc_collect", "logger_message_routing", read_routing, "EOC for model"},
        {"coor_routing_ic_collect", "logger_message_routing", read_routing, "IC for model"},
        {"coor_routing_eic_collect", "logger_message_routing", read_routing, "EIC for model"},
        // the dynamic engine has ONE routing-detail event (coor_routing_collect) for IC, EIC and EOC alike
        {"coor_routing_collect", "logger_message_routing", read_routing, " in port greenhouse::heater_defs::command has {heater ON"},
        {"sim_local_time", "logger_local_time", read_local, "Elapsed in model"},
        {"run_global_time", "logger_global_time", read_global, "5\n"},
    };
    for (const auto& row : dynamic_rows)
        CHECK_MSG(has(row.sink(), row.needle), std::string(row.event) + "  (" + row.source + ")  -> \"" + row.needle + "\"");
    std::cout << "    routing detail (dynamic): "
              << read_routing().substr(read_routing().find(" in port"), 100) << "\n";

    // =====================================================================================
    section("2. custom sinks and formatters");
    // =====================================================================================
    // (a) a FILE sink + our trace_formatter: state lines of the whole run into a file
    struct file_tag {};
    const std::string path = "build/out/05_state_trace.txt";
    {
        std::system("mkdir -p build/out");
        file_sink<file_tag>::open(path);
        using file_logger = lg::multilogger<
            lg::logger<lg::logger_global_time, trace_formatter<float>, file_sink<file_tag>>,
            lg::logger<lg::logger_state, trace_formatter<float>, file_sink<file_tag>>>;
        cadmium::engine::runner<float, climate_standalone, file_logger> runner{0.0f};
        runner.run_until(3.0f);
        file_sink<file_tag>::close();
    }
    std::ifstream in(path);
    std::string line, all;
    int lines = 0;
    while (std::getline(in, line)) { all += line + "\n"; ++lines; }
    CHECK(lines > 10);
    CHECK(has(all, "ST\tgreenhouse::heater<float>\tOFF sigma=inf pulses=0"));
    CHECK(has(all, "T\t2.5"));
    std::cout << "  " << lines << " lines written to " << path << "; e.g. \"" << all.substr(0, all.find('\n')) << "\"\n";

    // (b) redirecting Cadmium's own console sinks: cout_sink_provider / cerr_sink_provider
    {
        using to_cout = lg::logger<lg::logger_info, lg::formatter<float>, lg::cout_sink_provider>;
        using to_cerr = lg::logger<lg::logger_info, lg::formatter<float>, lg::cerr_sink_provider>;
        std::ostringstream out_capture, err_capture;
        auto* old_out = std::cout.rdbuf(out_capture.rdbuf());
        auto* old_err = std::cerr.rdbuf(err_capture.rdbuf());
        to_cout::log<lg::logger_info, lg::run_info>(std::string("hello stdout"));
        to_cerr::log<lg::logger_info, lg::run_info>(std::string("hello stderr"));
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        CHECK_EQ(out_capture.str(), std::string("hello stdout\n"));
        CHECK_EQ(err_capture.str(), std::string("hello stderr\n"));
    }

    // (c) a logger only reacts to ITS source: this one is bound to logger_state, so an info
    //     event sent to it is discarded at compile time (if constexpr), nothing is written.
    {
        using only_state = lg::logger<lg::logger_state, lg::formatter<float>, memory_sink<tag_debug>>;
        memory_sink<tag_debug>::clear();
        only_state::log<lg::logger_info, lg::run_info>(std::string("ignored"));
        CHECK(memory_sink<tag_debug>::contents().empty());
        only_state::log<lg::logger_state, lg::sim_state>(std::string("S"), std::string("model"));
        CHECK_EQ(memory_sink<tag_debug>::contents(), std::string("State for model model is S\n"));
    }

    // =====================================================================================
    section("3. the null logger and the verbatim formatter");
    // =====================================================================================
    {
        // not_logger = logger<not_matching_source, ...>: no engine event ever uses that source,
        // so passing it as the LOGGER switches logging off entirely (zero overhead).
        std::ostringstream captured;
        auto* old = std::cout.rdbuf(captured.rdbuf());
        {
            cadmium::engine::runner<float, climate_standalone, lg::not_logger> runner{0.0f};
            runner.run_until(3.0f);
        }
        std::cout.rdbuf(old);
        CHECK(captured.str().empty());

        // verbatim_formatter: prints its arguments one after the other; a callable argument is
        // invoked with the arguments that follow it.
        std::ostringstream os;
        lg::verbatim_formatter::format(os, "answer: ", 42, ' ', 1.5);
        CHECK_EQ(os.str(), std::string("answer: 42 1.5\n"));
        std::ostringstream os2;
        lg::verbatim_formatter::format(os2, [](int a, int b) { return a + b; }, 20, 22);
        CHECK_EQ(os2.str(), std::string("42\n"));
        std::ostringstream os3;
        lg::verbatim_formatter::format(os3);  // no arguments: just the newline
        CHECK_EQ(os3.str(), std::string("\n"));
        // is_callable<F(Args...)> is the trait verbatim_formatter uses to tell "a callable" from "a value"
        auto adder = [](int a, int b) { return a + b; };
        static_assert(lg::is_callable<decltype(adder)(int, int)>::value, "a lambda is callable with two ints");
        static_assert(!lg::is_callable<int(int)>::value, "an int is not callable");
        static_assert(lg::is_callable_v<decltype(adder)(int, int)>, "variable template form");
        CHECK_MSG(true, "is_callable / is_callable_v classify callables at compile time (static_assert)");
    }

    // =====================================================================================
    section("4. message printing helpers");
    // =====================================================================================
    {
        static_assert(lg::is_streamable<int>::value, "int is streamable");
        static_assert(lg::is_streamable<heater_command>::value, "heater_command has operator<<");
        static_assert(!lg::is_streamable<unprintable>::value, "unprintable has none");
        static_assert(!lg::is_streamable<report_tick_msg>::value, "neither has the stock reset_tick");
        CHECK_MSG(true, "is_streamable<T> detects operator<< at compile time (static_assert)");

        std::ostringstream a, b;
        lg::value_or_name<int>::print(a, 7);                 // streamable: the value
        lg::value_or_name<unprintable>::print(b, unprintable{});  // not streamable: "obscure message of type ..."
        CHECK_EQ(a.str(), std::string("7"));
        CHECK(has(b.str(), "obscure message of type") && has(b.str(), "unprintable"));

        std::ostringstream c, d;
        lg::implode(c, std::vector<int>{1, 2, 3});           // {1, 2, 3}
        lg::implode(d, std::vector<int>{});                  // {}
        CHECK_EQ(c.str(), std::string("{1, 2, 3}"));
        CHECK_EQ(d.str(), std::string("{}"));
        auto strings = lg::messages_as_strings(std::vector<heater_command>{{power::on, 1.0f}, {power::off, 2.0f}});
        CHECK_EQ(strings.size(), 2u);
        CHECK_EQ(strings[1], std::string("heater OFF (reading 2)"));

        // print_messages_by_port: "[port: {messages}, port: {messages}]" for a tuple of bags
        cadmium::make_message_bags<thermostat<float>::output_ports>::type out_bags;
        cadmium::get_messages<thermostat_defs::heater_command_out>(out_bags).push_back(heater_command{power::on, 17.0f});
        std::ostringstream e;
        lg::print_messages_by_port(e, out_bags);
        std::cout << "  " << e.str() << "\n";
        CHECK_EQ(e.str(), std::string("[greenhouse::thermostat_defs::heater_command_out: {heater ON (reading 17)}, "
                                      "greenhouse::thermostat_defs::overheat_alarm: {}]"));
    }

    // =====================================================================================
    section("5. default loggers and formatter details");
    // =====================================================================================
    {
        // What a runner uses when you do not pass a LOGGER: model *states* to std::cout.
        static_assert(std::is_same<cadmium::engine::default_logger<float>,
                                   lg::logger<lg::logger_state, lg::formatter<float>, lg::cout_sink_provider>>::value,
                      "static default logger");
        static_assert(std::is_same<cadmium::dynamic::engine::default_logger<float>,
                                   lg::logger<lg::logger_state, cadmium::dynamic::logger::formatter<float>,
                                              lg::cout_sink_provider>>::value,
                      "dynamic default logger");
        CHECK_MSG(true, "default_logger = logger<logger_state, formatter<TIME>, cout_sink_provider> for both runners");

        // Formatters are templated on TIME, so any TIME type that streams works, e.g. fixed_time
        CHECK_EQ(lg::formatter<fixed_time>::sim_info_init(fixed_time(2.5), "m"),
                 std::string("Simulator for model m initialized to time 2.5"));
        CHECK_EQ(lg::formatter<fixed_time>::sim_local_time(fixed_time(1.0), fixed_time(3.5), "m"),
                 std::string("Elapsed in model m is 2.5s"));
        // the static and dynamic formatters word the common events identically
        CHECK_EQ(lg::formatter<float>::sim_state("S", "m"), std::string("State for model m is S"));
        CHECK_EQ(cadmium::dynamic::logger::formatter<float>::sim_state(1.0f, "m", "S"), std::string("State for model m is S"));
        // ... but not every event has the same arguments: the dynamic one routes with ONE event
        CHECK_EQ(cadmium::dynamic::logger::formatter<float>::coor_routing_collect("out", "in", {"1"}, {"1", "1"}),
                 std::string(" in port in has {1, 1} routed from out with messages {1}"));
        CHECK_EQ(lg::formatter<float>::run_global_time(2.5f), 2.5f);
        CHECK_EQ(lg::formatter<float>::run_info("note"), std::string("note"));
    }

    // =====================================================================================
    section("6. the user-level logger_debug channel");
    // =====================================================================================
    {
        std::ostringstream captured;
        debug_note("dropped: the default target is a null stream");
        set_debug_target(&captured);
        debug_note("thermostat says ", 17.5, " degrees");
        debug_logger::log<lg::logger_debug, lg::run_info>(std::string("direct call"));
        set_debug_target(nullptr);
        debug_note("dropped again");
        CHECK_EQ(captured.str(), std::string("thermostat says 17.5 degrees\ndirect call\n"));
    }

    // =====================================================================================
    section("7. thread safety of the project's sinks (needed by the concurrent / OpenMP engines)");
    // =====================================================================================
    {
        struct stress_tag {};
        memory_sink<stress_tag>::clear();
        const int threads = 8, per_thread = 2000;
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([t, per_thread] {
                for (int i = 0; i < per_thread; ++i) {
                    // exactly how Cadmium's logger writes a line: two separate operations
                    memory_sink<stress_tag>::sink() << "thread " << t << " line " << i;
                    memory_sink<stress_tag>::sink() << std::endl;
                }
            });
        for (auto& th : pool) th.join();
        std::istringstream in2(memory_sink<stress_tag>::contents());
        std::string l;
        int total = 0, torn = 0;
        while (std::getline(in2, l)) {
            ++total;
            int t = -1, i = -1;
            if (std::sscanf(l.c_str(), "thread %d line %d", &t, &i) != 2 || t < 0 || t >= threads) ++torn;
        }
        CHECK_EQ(total, threads * per_thread);
        CHECK_EQ(torn, 0);
        std::cout << "  " << total << " lines from " << threads << " threads, none torn or lost\n";

        locked_stream shared;  // the general-purpose thread-safe stream (used for the debug channel)
        std::vector<std::thread> pool2;
        for (int t = 0; t < threads; ++t)
            pool2.emplace_back([&shared, per_thread] {
                for (int i = 0; i < per_thread; ++i) shared << 'x';
            });
        for (auto& th : pool2) th.join();
        CHECK_EQ(shared.str().size(), static_cast<std::size_t>(threads * per_thread));
        shared.clear_contents();
        CHECK(shared.str().empty());
    }

    return check_summary("05_logging_tour");
}
