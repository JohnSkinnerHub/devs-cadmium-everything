/**
 * @file thermostat.hpp
 * @brief The greenhouse thermostat: a hysteresis controller with a decision latency.
 *
 * WHAT IT DOES
 *   Listens to temperature readings. After a short "decision delay" it decides whether the
 *   heater must be switched ON (too cold), OFF (warm enough) or left alone, and raises an
 *   overheat alarm if the reading is above a critical limit.
 *
 *     reading < low_c      -> command heater ON   (if it is not already on)
 *     reading > high_c     -> command heater OFF  (if it is not already off)
 *     reading > critical_c -> alarm (an int on the alarm port)
 *     otherwise            -> do nothing (the "hysteresis band" avoids chattering)
 *
 * WHY IT IS INTERESTING FROM A DEVS POINT OF VIEW
 *   The decision latency makes this model exercise **all three** kinds of transition, which
 *   is the point of using it as the centre piece of the demo:
 *
 *   - delta_ext : a reading arrives while idle -> start a decision timer;
 *                 a reading arrives while a timer is running -> keep the ORIGINAL deadline
 *                 (it uses the elapsed time `e` to shorten the remaining time) but refresh the
 *                 reading, so the controller always decides on the freshest data.
 *   - delta_int : the timer expires -> commit the decision, go idle.
 *   - delta_con : a reading arrives at *exactly* the instant the timer expires.
 *                 Parallel DEVS makes the modeller say what happens. We choose the standard
 *                 "serial" semantics: first finish the old decision (delta_int), then absorb
 *                 the new reading with zero elapsed time: delta_con = delta_ext(delta_int(s), 0, x).
 *
 * FORMAL SPECIFICATION (Parallel DEVS, Chow & Zeigler 1994)
 *   M = < X, S, Y, delta_int, delta_ext, delta_con, lambda, ta >
 *
 *   X  = { (reading, v) : v in float }                      -- a bag per input port
 *   Y  = { (heater_command, c) } U { (overheat_alarm, 1) }  -- a bag per output port
 *   S  = { (mode, reading, commanded, sigma) :
 *           mode in {idle, deciding}, reading in R, commanded in {off,on}, sigma in R+ U {inf} }
 *   initial s0 = (idle, 0, off, inf)
 *
 *   ta(s)  = sigma                                   (sigma = inf when idle)
 *   want(r, c) = on  if r < low and c = off
 *                off if r > high and c = on
 *                c   otherwise
 *   lambda(s) = { heater_command(want(reading, commanded)) if want != commanded,
 *                 alarm(1)                                  if reading > critical }   (only when deciding)
 *   delta_int(s)  = (idle, reading, want(reading, commanded), inf)
 *   delta_ext(s, e, x):
 *        reading' = last value in the bag for port `reading`
 *        if mode = idle      then (deciding, reading', commanded, decision_delay)
 *        if mode = deciding  then (deciding, reading', commanded, sigma - e)
 *   delta_con(s, x) = delta_ext(delta_int(s), 0, x)
 *
 * HOW IT IS WRITTEN IN CADMIUM
 *   - A class template on TIME, so the runner (not the model) chooses float/double/fixed_time.
 *   - `state_type` + public `state`  (the engines print `state` after every transition).
 *   - `input_ports` / `output_ports` are tuples of port *types*; the engine builds the
 *     message bags from them with `make_message_bags`.
 *   - the five functions: internal_transition, external_transition, confluence_transition,
 *     output (= lambda), time_advance (= ta).
 *   - a non-default constructor taking a config, used by the dynamic engine demo to show that
 *     `make_dynamic_atomic_model<...>(id, args...)` forwards constructor arguments.
 */
#ifndef GREENHOUSE_MODELS_THERMOSTAT_HPP
#define GREENHOUSE_MODELS_THERMOSTAT_HPP

#include "../prelude.hpp"

#include <limits>
#include <ostream>
#include <tuple>

#include <cadmium/modeling/message_bag.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../debug_log.hpp"
#include "../messages.hpp"

namespace greenhouse {

/// Tunable parameters of the controller (temperatures in degrees Celsius).
struct thermostat_config {
    double low_c = 18.0;             ///< below this the heater is switched ON
    double high_c = 24.0;            ///< above this the heater is switched OFF
    double critical_c = 35.0;        ///< above this the overheat alarm is raised
    double decision_delay_s = 0.5;   ///< time between a reading and the resulting command
};

/// Port definitions. Grouping them in a struct is the convention of Cadmium's stock models.
struct thermostat_defs {
    struct reading : public cadmium::in_port<float> {};
    struct heater_command_out : public cadmium::out_port<heater_command> {};
    struct overheat_alarm : public cadmium::out_port<int> {};
};

/// The state `S` of the thermostat. Defined at namespace scope so that its `operator<<`
/// is found by argument-dependent lookup from inside the engines.
template <typename TIME>
struct thermostat_state {
    enum class phase { idle, deciding };
    phase mode = phase::idle;
    float reading = 0.0f;
    power commanded = power::off;
    TIME sigma = std::numeric_limits<TIME>::infinity();  ///< time left until the decision
};

template <typename TIME>
std::ostream& operator<<(std::ostream& os, const thermostat_state<TIME>& s) {
    os << (s.mode == thermostat_state<TIME>::phase::idle ? "idle" : "deciding")
       << " reading=" << s.reading << " commanded=" << s.commanded << " sigma=" << s.sigma;
    return os;
}

template <typename TIME>
class thermostat {
    using defs = thermostat_defs;

public:
    using state_type = thermostat_state<TIME>;
    state_type state;
    thermostat_config config;

    using input_ports = std::tuple<typename defs::reading>;
    using output_ports = std::tuple<typename defs::heater_command_out, typename defs::overheat_alarm>;

    thermostat() = default;
    explicit thermostat(const thermostat_config& cfg) : config(cfg) {}

    // ---- delta_int --------------------------------------------------------------------
    void internal_transition() {
        state.commanded = want(state.reading, state.commanded);
        state.mode = state_type::phase::idle;
        state.sigma = std::numeric_limits<TIME>::infinity();
        debug_note("thermostat: decision committed, heater ", state.commanded);
    }

    // ---- delta_ext --------------------------------------------------------------------
    void external_transition(TIME e, typename cadmium::make_message_bags<input_ports>::type mbs) {
        const auto& readings = cadmium::get_messages<typename defs::reading>(mbs);
        if (!readings.empty()) state.reading = readings.back();  // freshest value wins
        if (state.mode == state_type::phase::idle) {
            state.mode = state_type::phase::deciding;
            state.sigma = TIME(config.decision_delay_s);
        } else {
            state.sigma = state.sigma - e;  // keep the original deadline
        }
        debug_note("thermostat: reading ", state.reading, " received, deciding for ", state.sigma, "s");
    }

    // ---- delta_con --------------------------------------------------------------------
    void confluence_transition(TIME /*e*/, typename cadmium::make_message_bags<input_ports>::type mbs) {
        debug_note("thermostat: CONFLUENCE (reading arrives exactly when the decision is due)");
        internal_transition();
        external_transition(TIME{}, std::move(mbs));
    }

    // ---- lambda -----------------------------------------------------------------------
    typename cadmium::make_message_bags<output_ports>::type output() const {
        typename cadmium::make_message_bags<output_ports>::type bags;
        if (state.mode == state_type::phase::deciding) {
            const power decided = want(state.reading, state.commanded);
            if (decided != state.commanded) {
                cadmium::get_messages<typename defs::heater_command_out>(bags)
                    .push_back(heater_command{decided, state.reading});
            }
            if (state.reading > config.critical_c) {
                cadmium::get_messages<typename defs::overheat_alarm>(bags).push_back(1);
            }
        }
        return bags;
    }

    // ---- ta ---------------------------------------------------------------------------
    TIME time_advance() const {
        return state.mode == state_type::phase::deciding ? state.sigma
                                                         : std::numeric_limits<TIME>::infinity();
    }

private:
    /// The control law (hysteresis band).
    power want(float reading, power current) const {
        if (reading < config.low_c && current == power::off) return power::on;
        if (reading > config.high_c && current == power::on) return power::off;
        return current;
    }
};

}  // namespace greenhouse

#endif  // GREENHOUSE_MODELS_THERMOSTAT_HPP
