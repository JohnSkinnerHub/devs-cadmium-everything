/**
 * @file 01_atomic_by_hand.cpp
 * @brief Cadmium's lowest level: ports, message bags and atomic models driven BY HAND.
 *
 * WHY THIS DEMO COMES FIRST
 *   An atomic DEVS model is just an object with a state and five functions. A simulator is
 *   "only" the thing that calls them in the right order at the right time. Before any engine
 *   is involved we can play simulator ourselves, which is the best way to understand what the
 *   engines in demos 02-04 are doing on our behalf.
 *
 * WHAT THIS DEMO SHOWS
 *   A. Ports and messages:  in_port, out_port, port_kind, message_bag, bag, make_message_bags,
 *                           get_messages, message_box, make_message_box, get_message.
 *   B. The five DEVS functions on the thermostat and heater (including the elapsed-time
 *      argument and the confluent transition).
 *   C. Every stock atomic model of Cadmium's pdevs library, driven by hand:
 *      generator (abstract), int_generator_one_sec, reset_generator_five_sec, accumulator,
 *      passive, filter_first_output, iestream_input + Parser.
 *   D. Compile-time concept checks: atomic_model_assert, coupled_model_assert, is_atomic and the
 *      helper traits behind them.
 *   E. Small utilities: tuple printing, helper::for_each, helper::join.
 */
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "greenhouse/check.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/models/heater.hpp"
#include "greenhouse/models/stock_aliases.hpp"
#include "greenhouse/models/thermostat.hpp"
#include "greenhouse/systems/static_greenhouse.hpp"

#include <cadmium/concept/atomic_model_assert.hpp>
#include <cadmium/concept/concept_helpers.hpp>
#include <cadmium/concept/coupled_model_assert.hpp>
#include <cadmium/engine/common_helpers.hpp>
#include <cadmium/modeling/message_box.hpp>

using namespace greenhouse;
using cadmium::get_messages;
using cadmium::make_message_bags;

// A concrete model derived from the stock *abstract* generator: it only has to say how often
// to emit (`period`) and what (`output_message`). Everything else is inherited.
template <typename TIME>
class heartbeat : public stock::generator<std::string, TIME> {
public:
    TIME period() const override { return 0.25; }
    std::string output_message() const override { return "beat"; }
};

// Ports used by section A (file scope so that they can be template arguments everywhere).
struct temperature_in : public cadmium::in_port<float> {};
struct count_in : public cadmium::in_port<int> {};
struct alarm_out : public cadmium::out_port<int> {};

int main() {
    // =====================================================================================
    section("A. ports, message bags and message boxes");
    // =====================================================================================
    // A *port* is a type. It says whether it is an input or an output, and what it carries.
    static_assert(temperature_in::kind == cadmium::port_kind::in, "in_port::kind");
    static_assert(alarm_out::kind == cadmium::port_kind::out, "out_port::kind");
    static_assert(std::is_same<temperature_in::message_type, float>::value, "in_port::message_type");
    CHECK_MSG(true, "in_port<float>::kind == port_kind::in, out_port<int>::kind == port_kind::out (static_assert)");

    // A *message_bag* holds the messages that arrived (or are sent) on ONE port at one instant.
    // PDEVS lets many messages share a port at the same instant, hence a bag, not a single value.
    cadmium::message_bag<temperature_in> one_bag{17.0f, 18.5f};  // initializer-list constructor
    CHECK_EQ(one_bag.messages.size(), 2u);
    cadmium::bag<float> raw = one_bag.messages;  // `bag<T>` is just std::vector<T>
    CHECK_EQ(raw.back(), 18.5f);
    cadmium::message_bag<temperature_in> empty_bag;  // default constructor: no messages
    CHECK(empty_bag.messages.empty());

    // The bags of a whole model: one bag per port, packed in a tuple built from the port list.
    using in_ports = std::tuple<temperature_in, count_in>;
    using in_bags = make_message_bags<in_ports>::type;  // tuple<message_bag<temperature_in>, message_bag<count_in>>
    in_bags bags;
    get_messages<temperature_in>(bags).push_back(21.5f);  // non-const overload: reference to the vector
    get_messages<count_in>(bags).push_back(7);
    const in_bags& const_view = bags;
    CHECK_EQ(get_messages<temperature_in>(const_view).front(), 21.5f);  // const overload
    CHECK_EQ(get_messages<count_in>(const_view).size(), 1u);

    // Classic DEVS allows at most ONE message per port: a *message_box* holds an optional value.
    using out_box = cadmium::make_message_box<std::tuple<alarm_out>>::type;  // tuple<message_box<alarm_out>>
    out_box box;
    CHECK(!cadmium::get_message<alarm_out>(box).has_value());
    cadmium::get_message<alarm_out>(box).emplace(3);  // non-const overload
    const out_box& const_box = box;
    CHECK_EQ(cadmium::get_message<alarm_out>(const_box).value(), 3);  // const overload

    // =====================================================================================
    section("B. the five DEVS functions, called by hand on the thermostat");
    // =====================================================================================
    {
        using T = float;
        thermostat<T> thermo;  // state s0 = (idle, reading 0, heater off, sigma = infinity)
        using thermo_in = thermostat<T>::input_ports;
        using thermo_out = thermostat<T>::output_ports;
        CHECK_EQ(thermo.time_advance(), std::numeric_limits<T>::infinity());  // ta(s0) = inf: passive

        // delta_ext at elapsed e=0: a reading of 17 degrees arrives (below the 18 limit)
        make_message_bags<thermo_in>::type x;
        get_messages<thermostat_defs::reading>(x).push_back(17.0f);
        thermo.external_transition(0.0f, x);
        CHECK_EQ(thermo.time_advance(), 0.5f);  // now deciding; decision due in 0.5 s

        // lambda(s) is computed BEFORE delta_int: it says what will be sent when the timer expires.
        make_message_bags<thermo_out>::type y = thermo.output();
        CHECK_EQ(get_messages<thermostat_defs::heater_command_out>(y).size(), 1u);
        CHECK(get_messages<thermostat_defs::heater_command_out>(y).front().setting == power::on);
        CHECK(get_messages<thermostat_defs::overheat_alarm>(y).empty());

        // A new reading arrives 0.2 s later: the elapsed time shortens the remaining time (0.5-0.2)
        get_messages<thermostat_defs::reading>(x).clear();
        get_messages<thermostat_defs::reading>(x).push_back(16.0f);
        thermo.external_transition(0.2f, x);
        CHECK_NEAR(thermo.time_advance(), 0.3, 1e-6);
        CHECK_EQ(thermo.state.reading, 16.0f);  // ... but the reading is refreshed

        // delta_int: the timer expires, the decision is committed
        thermo.internal_transition();
        CHECK(thermo.state.commanded == power::on);
        CHECK_EQ(thermo.time_advance(), std::numeric_limits<T>::infinity());

        // delta_con: an input arrives at the very instant the timer expires
        thermostat<T> conf;
        get_messages<thermostat_defs::reading>(x).clear();
        get_messages<thermostat_defs::reading>(x).push_back(17.0f);
        conf.external_transition(0.0f, x);  // deciding, due in 0.5
        get_messages<thermostat_defs::reading>(x).clear();
        get_messages<thermostat_defs::reading>(x).push_back(30.0f);
        conf.confluence_transition(0.5f, x);       // = delta_int (heater ON) then delta_ext(0, 30)
        CHECK(conf.state.commanded == power::on);  // the OLD decision was committed first ...
        CHECK_EQ(conf.state.reading, 30.0f);       // ... and the new reading is now being processed
        CHECK_EQ(conf.time_advance(), 0.5f);

        // the model prints through its state's operator<<, this is what the engines log
        std::ostringstream oss;
        oss << conf.state;
        std::cout << "  state printed by the engines: " << oss.str() << "\n";
        CHECK_EQ(oss.str(), std::string("deciding reading=30 commanded=ON sigma=0.5"));

        // overheat: above the critical limit the alarm port is used as well
        thermostat<T> hot;
        get_messages<thermostat_defs::reading>(x).clear();
        get_messages<thermostat_defs::reading>(x).push_back(40.0f);
        hot.external_transition(0.0f, x);
        CHECK_EQ(get_messages<thermostat_defs::overheat_alarm>(hot.output()).size(), 1u);

        // thresholds are configurable through the constructor (used by the dynamic demo)
        thermostat_config cfg;
        cfg.high_c = 20.0;
        cfg.decision_delay_s = 2.0;
        thermostat<T> custom(cfg);
        get_messages<thermostat_defs::reading>(x).clear();
        get_messages<thermostat_defs::reading>(x).push_back(25.0f);
        custom.external_transition(0.0f, x);
        CHECK_EQ(custom.time_advance(), 2.0f);

        // the same model with the custom time type
        thermostat<fixed_time> exact;
        make_message_bags<thermostat<fixed_time>::input_ports>::type xf;
        get_messages<thermostat_defs::reading>(xf).push_back(10.0f);
        exact.external_transition(fixed_time{}, xf);
        CHECK(exact.time_advance() == fixed_time(0.5));
    }

    section("B2. the heater: elapsed time in action");
    {
        heater<float> h;
        make_message_bags<heater<float>::input_ports>::type x;
        get_messages<heater_defs::command>(x).push_back(heater_command{power::on, 15.0f});
        h.external_transition(0.0f, x);
        CHECK_EQ(h.time_advance(), 1.0f);  // ON: first pulse in one period
        // the same ON command again, 0.4 s into the period: the schedule is NOT restarted
        h.external_transition(0.4f, x);
        CHECK_NEAR(h.time_advance(), 0.6, 1e-6);
        CHECK_EQ(get_messages<heater_defs::energy_pulse>(h.output()).front(), 1);  // lambda: one energy unit
        h.internal_transition();  // the pulse has been emitted
        CHECK_EQ(h.state.pulses, 1);
        CHECK_EQ(h.time_advance(), 1.0f);

        // OFF command in the middle of a period: no more pulses
        get_messages<heater_defs::command>(x).clear();
        get_messages<heater_defs::command>(x).push_back(heater_command{power::off, 26.0f});
        h.external_transition(0.3f, x);
        CHECK_EQ(h.time_advance(), std::numeric_limits<float>::infinity());

        // heater_command is a user message type: it has operator== and operator<<
        std::ostringstream oss;
        oss << heater_command{power::on, 17.0f};
        CHECK_EQ(oss.str(), std::string("heater ON (reading 17)"));
        CHECK((heater_command{power::on, 17.0f} == heater_command{power::on, 17.0f}));
    }

    // =====================================================================================
    section("C. every stock atomic model, driven by hand");
    // =====================================================================================
    {
        // ---- int_generator_one_sec: emits the int 1 every second, has no input ports ----
        stock::int_generator_one_sec<float> one_sec;
        CHECK_EQ(one_sec.period(), 1.0f);
        CHECK_EQ(one_sec.output_message(), 1);
        CHECK_EQ(one_sec.time_advance(), 1.0f);
        CHECK_EQ(get_messages<stock::int_generator_one_sec_defs::out>(one_sec.output()).front(), 1);
        one_sec.internal_transition();  // does nothing: the generator has no state to change
        using no_in = make_message_bags<stock::int_generator_one_sec<float>::input_ports>::type;  // empty tuple
        CHECK_THROWS(one_sec.external_transition(0.0f, no_in{}), std::logic_error);
        CHECK_THROWS(one_sec.confluence_transition(0.0f, no_in{}), std::logic_error);

        // ---- reset_generator_five_sec: emits a reset_tick every 5 seconds ----
        stock::reset_generator_five_sec<float> five_sec;
        CHECK_EQ(five_sec.time_advance(), 5.0f);
        CHECK_EQ(get_messages<stock::reset_generator_five_sec_defs::out>(five_sec.output()).size(), 1u);
        // (a reset_tick is an empty struct: there is nothing to compare, its *arrival* is the message)
        five_sec.internal_transition();
        CHECK_THROWS(five_sec.external_transition(0.0f, no_in{}), std::logic_error);
        CHECK_THROWS(five_sec.confluence_transition(0.0f, no_in{}), std::logic_error);

        // ---- generator<VALUE,TIME>: abstract; heartbeat fills in period() and output_message() ----
        heartbeat<float> beat;
        CHECK_EQ(beat.time_advance(), 0.25f);
        CHECK_EQ(get_messages<stock::generator_defs<std::string>::out>(beat.output()).front(), std::string("beat"));
        CHECK_THROWS(beat.external_transition(0.0f, no_in{}), std::logic_error);
        CHECK_THROWS(beat.confluence_transition(0.0f, no_in{}), std::logic_error);
        beat.internal_transition();

        // ---- accumulator<VALUE,TIME>: sums `add`, reports the sum when it receives `reset` ----
        stock::accumulator<int, float> acc;
        using acc_in = stock::accumulator<int, float>::input_ports;
        using acc_defs = stock::accumulator_defs<int>;
        CHECK_EQ(acc.time_advance(), std::numeric_limits<float>::infinity());  // idle: waits for input
        make_message_bags<acc_in>::type adds;
        for (int v : {1, 2, 3}) get_messages<acc_defs::add>(adds).push_back(v);
        acc.external_transition(0.0f, adds);
        CHECK_EQ(std::get<int>(acc.state), 6);
        CHECK_THROWS(acc.output(), std::logic_error);          // may only output after a reset request
        CHECK_THROWS(acc.internal_transition(), std::logic_error);
        make_message_bags<acc_in>::type reset_only;
        get_messages<acc_defs::reset>(reset_only).push_back(acc_defs::reset_tick{});
        acc.external_transition(1.0f, reset_only);
        CHECK(std::get<bool>(acc.state));                      // on_reset
        CHECK_EQ(acc.time_advance(), 0.0f);                    // report immediately
        CHECK_EQ(get_messages<acc_defs::sum>(acc.output()).front(), 6);
        CHECK_THROWS(acc.external_transition(0.0f, adds), std::logic_error);  // busy reporting
        // delta_con = delta_int (zero the sum) then delta_ext (take the new input)
        acc.confluence_transition(0.0f, adds);
        CHECK_EQ(std::get<int>(acc.state), 6);                 // 0 after reset, +1+2+3 = 6
        CHECK(!std::get<bool>(acc.state));
        // delta_int on its own
        acc.external_transition(0.0f, reset_only);
        acc.internal_transition();
        CHECK_EQ(std::get<int>(acc.state), 0);
        CHECK(!std::get<bool>(acc.state));

        // reset_generator_five_sec.hpp also defines handy aliases for the accumulator it is meant to reset
        static_assert(std::is_same<stock::test_accumulator<float>, stock::accumulator<int, float>>::value, "test_accumulator");
        static_assert(std::is_same<stock::test_accumulator_defs, stock::accumulator_defs<int>>::value, "test_accumulator_defs");
        static_assert(std::is_same<stock::reset_tick, stock::accumulator_defs<int>::reset_tick>::value, "reset_tick alias");

        // ---- passive<VALUE,TIME>: absorbs its input forever ----
        stock::passive<int, float> sink;
        make_message_bags<stock::passive<int, float>::input_ports>::type in_sink;
        get_messages<stock::passive_defs<int>::in>(in_sink).push_back(1);
        sink.external_transition(0.0f, in_sink);  // fine, does nothing
        CHECK_EQ(sink.time_advance(), std::numeric_limits<float>::infinity());
        CHECK_THROWS(sink.internal_transition(), std::logic_error);
        CHECK_THROWS(sink.confluence_transition(0.0f, in_sink), std::logic_error);
        CHECK_THROWS(sink.output(), std::logic_error);

        // ---- filter_first_output: after the FIRST input, emit one `1`; ignore everything later ----
        stock::filter_first_output<float> latch;
        make_message_bags<stock::filter_first_output<float>::input_ports>::type in_latch;
        get_messages<stock::filter_first_output_defs::in>(in_latch).push_back(99);
        CHECK_EQ(latch.time_advance(), std::numeric_limits<float>::infinity());
        latch.external_transition(0.0f, in_latch);  // state 1: fire now
        CHECK_EQ(latch.time_advance(), 0.0f);
        CHECK_EQ(get_messages<stock::filter_first_output_defs::out>(latch.output()).front(), 1);
        latch.internal_transition();                // state 2: done
        CHECK_EQ(latch.time_advance(), std::numeric_limits<float>::infinity());
        latch.external_transition(0.0f, in_latch);  // a second input is swallowed
        CHECK_EQ(latch.time_advance(), std::numeric_limits<float>::infinity());
        // (its confluence_transition is `assert(false)`: the model documents "never called", so we don't)

        // ---- Parser: the file reader inside iestream_input ----
        {
            stock::Parser<float, float> parser;  // default-constructed, then opened explicitly
            parser.open_file("data/sensor_feed.txt");
            auto first = parser.next_timed_input();
            CHECK(first.first == 1.0f && first.second == 21.0f);
            stock::Parser<float, float> direct("data/sensor_feed.txt");  // or open in the constructor
            int lines = 0;
            try {
                for (;;) {
                    direct.next_timed_input();
                    ++lines;
                }
            } catch (const std::exception&) {
                // end of file is signalled by an exception
            }
            CHECK_EQ(lines, 11);
        }

        // ---- iestream_input: replays the file as timed output messages ----
        stock::iestream_input<float, float> feed("data/sensor_feed.txt");
        std::vector<std::pair<double, float>> replay;  // (absolute time, value)
        double clock = 0;
        int steps = 0;
        while (feed.time_advance() != std::numeric_limits<float>::infinity() && steps++ < 100) {
            clock += feed.time_advance();
            const auto produced = feed.output();  // keep the bags alive: get_messages returns a reference into them
            for (float v : get_messages<stock::iestream_input_defs<float>::out>(produced)) replay.emplace_back(clock, v);
            feed.internal_transition();
        }
        std::cout << "  replayed (time, value):";
        for (auto& p : replay) std::cout << " (" << p.first << "," << p.second << ")";
        std::cout << "\n";
        CHECK_EQ(replay.size(), 11u);
        CHECK_NEAR(replay.front().first, 1.0, 1e-4);
        CHECK_EQ(replay.front().second, 21.0f);
        CHECK_NEAR(replay[5].first, 7.2, 1e-4);  // 6th line of the file: "7.2 38.0"
        CHECK_EQ(replay[5].second, 38.0f);
        CHECK_NEAR(replay.back().first, 17.0, 1e-3);
        CHECK_EQ(replay.back().second, 26.0f);
        CHECK_EQ(feed.time_advance(), std::numeric_limits<float>::infinity());  // exhausted
        std::ostringstream state_text;
        state_text << feed.state;  // its state prints through an operator<< for std::ostringstream
        CHECK_EQ(state_text.str(), std::string("next time: inf"));
    }

    // =====================================================================================
    section("D. compile-time concept checks (they are static_asserts: reaching run time = they passed)");
    // =====================================================================================
    {
        // An atomic model must define: state/state_type, input_ports, output_ports and the five
        // functions with the right signatures; ports must be unique. Cadmium checks the model
        // instantiated with `float` time, which is why every model must also compile with float.
        cadmium::concept::pdevs::atomic_model_assert<thermostat>();
        cadmium::concept::pdevs::atomic_model_assert<heater>();
        cadmium::concept::pdevs::atomic_model_float_time_assert<heater<float>>();  // same check, model type given
        static_assert(cadmium::concept::is_atomic<thermostat>::value(), "thermostat is atomic");
        static_assert(!cadmium::concept::is_atomic<irrigation_model>::value(), "irrigation is coupled");
        // A coupled model: ports, couplings and (recursively) every sub-model are checked.
        cadmium::concept::pdevs::coupled_model_assert<irrigation_model>();
        cadmium::concept::pdevs::coupled_model_assert<greenhouse_top>();
        cadmium::concept::pdevs::coupled_model_float_time_assert<climate_model<float>>();
        // The individual pieces of the coupled checks can be called directly:
        using irrigation = irrigation_model<float>;
        cadmium::concept::assert_eic(irrigation::input_ports{}, irrigation::external_input_couplings{});
        cadmium::concept::assert_eoc(irrigation::output_ports{}, irrigation::external_output_couplings{});
        cadmium::concept::assert_ic(irrigation::internal_couplings{});
        cadmium::concept::pdevs::assert_submodels<irrigation::models<float>>();
        // ... and so can the traits underneath them:
        static_assert(cadmium::concept::is_tuple<std::tuple<int, char>>(), "is_tuple");
        static_assert(!cadmium::concept::is_tuple<int>(), "is_tuple (negative)");
        static_assert(cadmium::concept::check_unique_elem_types<std::tuple<temperature_in, count_in>>::value(),
                      "check_unique_elem_types: all port types are different");
        static_assert(cadmium::concept::has_port_in_tuple<count_in, in_ports>::value(), "has_port_in_tuple");
        static_assert(!cadmium::concept::has_port_in_tuple<alarm_out, in_ports>::value(),
                      "has_port_in_tuple (negative)");
        CHECK_MSG(true, "atomic_model_assert, coupled_model_assert, is_atomic and the helper traits all passed");
        std::cout << "  (the failing cases live in tests/compile_fail: they must NOT compile)\n";
    }

    // =====================================================================================
    section("E. small utilities");
    // =====================================================================================
    {
        // tuples print as [a, b, c], this is how the accumulator's state shows up in logs
        std::ostringstream oss;
        {
            using cadmium::operator<<;  // declared in cadmium/logger/tuple_to_ostream.hpp
            oss << std::make_tuple(1, 2.5, 'x');
        }
        CHECK_EQ(oss.str(), std::string("[1, 2.5, x]"));

        // helper::for_each applies a callable to every element of a tuple (a fold over std::apply)
        int total = 0;
        auto numbers = std::make_tuple(1, 2, 3);
        cadmium::helper::for_each(numbers, [&total](int n) { total += n; });
        CHECK_EQ(total, 6);

        // helper::join prints a list of strings as {a, b, c}
        CHECK_EQ(cadmium::helper::join({"a", "b", "c"}), std::string("{a, b, c}"));
        CHECK_EQ(cadmium::helper::join({}), std::string("{}"));
    }

    return check_summary("01_atomic_by_hand");
}
