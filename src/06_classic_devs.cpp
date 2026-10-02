/**
 * @file 06_classic_devs.cpp
 * @brief Classic DEVS in Cadmium: the models, the SELECT function, and why Parallel DEVS exists.
 *
 * WHAT CADMIUM OFFERS FOR CLASSIC DEVS (and what it does not)
 *   Cadmium implements two formalisms. Everything in demos 01-05 is *Parallel* DEVS (PDEVS).
 *   For *classic* DEVS (Zeigler 1976) the library provides the MODELING side:
 *       cadmium::message_box / make_message_box / get_message   at most one message per port
 *       cadmium::basic_models::devs::{generator, accumulator, passive}
 *       cadmium::modeling::devs::coupling<..., SELECT>           a coupled model WITH a Select type
 *       cadmium::concept::devs::atomic_model_assert              compile-time model check
 *   ...but NO simulator for it (the engines in cadmium/engine are PDEVS only). So this demo
 *   brings a tiny sequential classic-DEVS driver of its own (`classic_driver` below) and uses it
 *   to show the one thing that really differs between the two formalisms: SIMULTANEOUS EVENTS.
 *
 * CLASSIC vs PARALLEL DEVS in one paragraph
 *   Classic DEVS atomic models have no confluent function and receive at most one message per
 *   port. If two components are imminent at the same instant, the coupled model must pick ONE
 *   of them to go first, that is the job of the `Select` function of the classic coupled model
 *   N = <X, Y, D, {M_d}, EIC, EOC, IC, Select>. The outcome can depend on that arbitrary choice.
 *   Parallel DEVS lets ALL imminent components fire together, and every model receives a *bag*
 *   of simultaneous inputs, with delta_con deciding what happens when internal and external
 *   events coincide. No arbitrary tie-break is needed.
 *
 * THE EXPERIMENT
 *   The irrigation counter of the greenhouse in classic DEVS: a pulse generator (1 s), a report
 *   generator (5 s) and an accumulator. At t = 5 the 5th pulse and the report request are
 *   simultaneous. Three Select policies give three different outcomes; PDEVS (demo 02) always
 *   reports 5.
 */
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "greenhouse/check.hpp"
#include "greenhouse/fixed_time.hpp"

#include <cadmium/basic_model/devs/accumulator.hpp>
#include <cadmium/basic_model/devs/generator.hpp>
#include <cadmium/basic_model/devs/passive.hpp>
#include <cadmium/concept/atomic_model_assert.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/message_box.hpp>

using namespace greenhouse;
namespace classic = cadmium::basic_models::devs;
using meter_ports = classic::accumulator_defs<int>;

// ---- classic atomic models --------------------------------------------------------------
// The stock classic generator is abstract: say how often and what to send.
template <typename TIME>
struct pulse_generator : public classic::generator<int, TIME> {
    TIME period() const override { return 1.0; }
    int output_message() const override { return 1; }
};
template <typename TIME>
struct report_generator : public classic::generator<meter_ports::reset_tick, TIME> {
    TIME period() const override { return 5.0; }
    meter_ports::reset_tick output_message() const override { return {}; }
};
template <typename TIME>
using classic_meter = classic::accumulator<int, TIME>;
template <typename TIME>
using classic_sink = classic::passive<int, TIME>;

// ---- the Select function: a type, as Cadmium's `devs::coupling` expects ------------------
// "Among the imminent components, which one goes first?": the first one in `priority`.
template <const char* const* PRIORITY, int N>
struct priority_select {
    static std::string choose(const std::vector<std::string>& imminent) {
        for (int i = 0; i < N; ++i)
            for (const auto& name : imminent)
                if (name == PRIORITY[i]) return name;
        return imminent.front();
    }
};
static const char* const kPulseFirst[] = {"pulse", "report", "meter", "sink"};
static const char* const kReportThenMeter[] = {"report", "meter", "pulse", "sink"};
static const char* const kReportThenPulse[] = {"report", "pulse", "meter", "sink"};
using select_pulse_first = priority_select<kPulseFirst, 4>;
using select_report_then_meter = priority_select<kReportThenMeter, 4>;
using select_report_then_pulse = priority_select<kReportThenPulse, 4>;

// ---- the classic coupled model: Cadmium's devs::coupling carries the Select type ---------
using no_ports = std::tuple<>;
template <typename SELECT>
struct classic_irrigation {
    template <typename TIME>
    using type = cadmium::modeling::devs::coupling<
        TIME, no_ports, no_ports,
        cadmium::modeling::models_tuple<pulse_generator, report_generator, classic_meter, classic_sink>,
        no_ports,  // EIC
        no_ports,  // EOC
        std::tuple<  // IC
            cadmium::modeling::IC<pulse_generator, classic::generator_defs<int>::out, classic_meter, meter_ports::add>,
            cadmium::modeling::IC<report_generator, classic::generator_defs<meter_ports::reset_tick>::out, classic_meter,
                                  meter_ports::reset>,
            cadmium::modeling::IC<classic_meter, meter_ports::sum, classic_sink, classic::passive_defs<int>::in>>,
        SELECT>;
};

/**
 * A minimal sequential CLASSIC DEVS simulator for the four models above (hand-wired).
 * It implements the classic abstract simulator:
 *     t := min next;  I := { d | next_d = t };  d* := Select(I)
 *     y := lambda_d*(s);  delta_int_d*;  every receiver r of y:  delta_ext_r(t - last_r, y);
 */
template <typename COUPLING>
class classic_driver {
    using T = double;
    pulse_generator<T> pulse_;
    report_generator<T> report_;
    classic_meter<T> meter_;
    classic_sink<T> sink_;
    std::map<std::string, T> last_{{"pulse", 0}, {"report", 0}, {"meter", 0}, {"sink", 0}};
    std::map<std::string, T> next_;

public:
    std::vector<std::pair<T, int>> reports;  // what the meter sent to the sink: (time, sum)
    std::vector<std::string> firing_order;   // "t:model", in the order components fired

    classic_driver() {
        next_["pulse"] = pulse_.time_advance();
        next_["report"] = report_.time_advance();
        next_["meter"] = meter_.time_advance();
        next_["sink"] = sink_.time_advance();
    }

    void run_until(T horizon) {
        for (;;) {
            T t = std::numeric_limits<T>::infinity();
            for (auto& kv : next_) t = std::min(t, kv.second);
            if (t >= horizon) return;
            std::vector<std::string> imminent;
            for (auto& kv : next_)
                if (kv.second == t) imminent.push_back(kv.first);
            // ---- the SELECT function taken from the coupled model's type ----
            const std::string d = COUPLING::select::choose(imminent);
            firing_order.push_back(std::to_string(static_cast<int>(t)) + ":" + d);
            fire(d, t);
        }
    }

private:
    template <typename M>
    void external(M& model, const std::string& name, T t, typename cadmium::make_message_box<typename M::input_ports>::type box) {
        model.external_transition(t - last_[name], box);
        last_[name] = t;
        next_[name] = t + model.time_advance();
    }
    void fire(const std::string& d, T t) {
        if (d == "pulse") {
            auto y = pulse_.output();  // lambda: a message_box with at most one message per port
            pulse_.internal_transition();
            last_[d] = t;
            next_[d] = t + pulse_.time_advance();
            cadmium::make_message_box<classic_meter<T>::input_ports>::type in;
            cadmium::get_message<meter_ports::add>(in) = cadmium::get_message<classic::generator_defs<int>::out>(y);
            external(meter_, "meter", t, in);  // IC: pulse -> meter.add
        } else if (d == "report") {
            auto y = report_.output();
            report_.internal_transition();
            last_[d] = t;
            next_[d] = t + report_.time_advance();
            cadmium::make_message_box<classic_meter<T>::input_ports>::type in;
            cadmium::get_message<meter_ports::reset>(in) =
                cadmium::get_message<classic::generator_defs<meter_ports::reset_tick>::out>(y);
            external(meter_, "meter", t, in);  // IC: report -> meter.reset
        } else if (d == "meter") {
            auto y = meter_.output();
            meter_.internal_transition();
            last_[d] = t;
            next_[d] = t + meter_.time_advance();
            reports.emplace_back(t, *cadmium::get_message<meter_ports::sum>(y));
            cadmium::make_message_box<classic_sink<T>::input_ports>::type in;
            cadmium::get_message<classic::passive_defs<int>::in>(in) = cadmium::get_message<meter_ports::sum>(y);
            external(sink_, "sink", t, in);  // IC: meter.sum -> sink
        }
    }
};

int main() {
    // =====================================================================================
    section("1. classic atomic models: message boxes instead of message bags");
    // =====================================================================================
    {
        pulse_generator<float> gen;
        auto box = gen.output();  // tuple<message_box<out>>: ONE optional message per port
        CHECK(cadmium::get_message<classic::generator_defs<int>::out>(box).has_value());
        CHECK_EQ(*cadmium::get_message<classic::generator_defs<int>::out>(box), 1);
        CHECK_EQ(gen.time_advance(), 1.0f);
        gen.internal_transition();
        cadmium::make_message_box<std::tuple<>>::type none;
        CHECK_THROWS(gen.external_transition(0.0f, none), std::logic_error);  // a generator has no inputs

        classic_meter<float> meter;
        cadmium::make_message_box<classic_meter<float>::input_ports>::type in;
        cadmium::get_message<meter_ports::add>(in).emplace(5);
        meter.external_transition(0.0f, in);  // one `add` message per port at most
        cadmium::get_message<meter_ports::add>(in).emplace(2);
        cadmium::get_message<meter_ports::reset>(in).emplace(meter_ports::reset_tick{});
        meter.external_transition(1.0f, in);  // add 2 and request the report
        CHECK_EQ(std::get<int>(meter.state), 7);
        CHECK_EQ(meter.time_advance(), 0.0f);
        CHECK_EQ(*cadmium::get_message<meter_ports::sum>(meter.output()), 7);
        meter.internal_transition();
        CHECK_EQ(std::get<int>(meter.state), 0);

        classic_sink<float> sink;
        cadmium::make_message_box<classic_sink<float>::input_ports>::type sink_in;
        cadmium::get_message<classic::passive_defs<int>::in>(sink_in).emplace(1);
        sink.external_transition(0.0f, sink_in);
        CHECK_EQ(sink.time_advance(), std::numeric_limits<float>::infinity());
        CHECK_THROWS(sink.internal_transition(), std::logic_error);
        CHECK_THROWS(sink.output(), std::logic_error);

        // compile-time validation of classic models: concept::devs::atomic_model_assert
        cadmium::concept::devs::atomic_model_assert<pulse_generator>();
        cadmium::concept::devs::atomic_model_assert<report_generator>();
        cadmium::concept::devs::atomic_model_assert<classic_meter>();
        cadmium::concept::devs::atomic_model_assert<classic_sink>();
        cadmium::concept::devs::atomic_model_float_time_assert<classic_meter<float>>();
        CHECK_MSG(true, "concept::devs::atomic_model_assert accepts all four classic models (static_assert)");
        // ... note that classic models have NO confluence_transition (it is a Parallel DEVS concept)
    }

    // =====================================================================================
    section("2. the classic coupled model carries a SELECT type");
    // =====================================================================================
    {
        using model_a = classic_irrigation<select_pulse_first>::type<float>;
        static_assert(std::is_same<model_a::select, select_pulse_first>::value, "coupling::select is the SELECT type");
        static_assert(std::tuple_size<model_a::internal_couplings>::value == 3, "three internal couplings");
        static_assert(std::tuple_size<model_a::models<float>>::value == 4, "four sub-models");
        CHECK_EQ(model_a::select::choose({"report", "pulse"}), std::string("pulse"));
        CHECK_EQ((classic_irrigation<select_report_then_meter>::type<float>::select::choose({"pulse", "report"})), std::string("report"));
        CHECK_MSG(true, "devs::coupling<..., SELECT> exposes `select`, `models`, `internal_couplings`, ... (static_assert)");
    }

    // =====================================================================================
    section("3. three Select policies, three different answers at t = 5");
    // =====================================================================================
    {
        // The report generator and the 5th pulse both fire at t=5.
        classic_driver<classic_irrigation<select_pulse_first>::type<double>> a;
        a.run_until(21.0);
        std::cout << "  Select = pulse before report        -> meter reports:";
        for (auto& r : a.reports) std::cout << " (" << r.first << "," << r.second << ")";
        std::cout << "\n";
        CHECK_EQ(a.reports.size(), 4u);
        for (auto& r : a.reports) CHECK_EQ(r.second, 5);  // pulse counted first: 5 each time, like PDEVS

        classic_driver<classic_irrigation<select_report_then_meter>::type<double>> b;
        b.run_until(21.0);
        std::cout << "  Select = report, then meter, then pulse -> meter reports:";
        for (auto& r : b.reports) std::cout << " (" << r.first << "," << r.second << ")";
        std::cout << "\n";
        CHECK_EQ(b.reports.size(), 4u);
        // The pulse that coincides with the report is counted AFTER it, so the first window
        // misses it (4) and every later window gets one extra (5): the 20 pulses are
        // attributed to windows differently, purely because of the tie-break.
        CHECK(b.reports[0].second == 4 && b.reports[1].second == 5 && b.reports[2].second == 5 &&
              b.reports[3].second == 5);

        classic_driver<classic_irrigation<select_report_then_pulse>::type<double>> c;
        // report first, then the pulse while the meter is busy reporting: the meter refuses
        CHECK_THROWS(c.run_until(21.0), std::logic_error);
        std::cout << "  Select = report, then pulse, then meter -> the accumulator throws "
                     "\"External transition called while on reset state\"\n";

        std::cout << "  firing order at t=5 with the first policy:";
        for (auto& f : a.firing_order)
            if (f.rfind("5:", 0) == 0) std::cout << " " << f;
        std::cout << "\n  Parallel DEVS (demo 02) reports 5 in every case: no Select, no arbitrary order.\n";
        CHECK(!a.firing_order.empty());
    }

    return check_summary("06_classic_devs");
}
