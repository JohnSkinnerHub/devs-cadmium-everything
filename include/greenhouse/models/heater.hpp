/**
 * @file heater.hpp
 * @brief The heater actuator: draws one unit of energy every `pulse_period` while it is ON.
 *
 * WHAT IT DOES
 *   Receives `heater_command`s. While ON it emits an `energy_pulse` (the integer 1, "one
 *   unit of energy used") once per pulse period. The pulses are accumulated by an energy
 *   meter (a stock Cadmium accumulator) elsewhere.
 *
 * WHY IT IS INTERESTING FROM A DEVS POINT OF VIEW
 *   - It is an *active* model with a periodic internal event, like a clock that can be
 *     started and stopped from outside.
 *   - When a command arrives mid-period the elapsed time `e` is essential: the time remaining
 *     until the next pulse is `sigma - e`. This is the classic use of the elapsed-time
 *     argument of delta_ext.
 *   - If an OFF command arrives at the very instant a pulse is due it is a **confluent**
 *     event: the pulse that was due is still emitted (lambda runs before delta_con) and
 *     then the heater switches off.
 *
 * FORMAL SPECIFICATION
 *   X = { (command, c) }    Y = { (energy_pulse, 1) }
 *   S = (on, sigma, pulses)  on in {false,true}, sigma in R+ U {inf}, pulses in N
 *   s0 = (false, inf, 0)
 *   ta(s)        = sigma if on, else inf
 *   lambda(s)    = { energy_pulse: 1 }
 *   delta_int(s) = (on, period, pulses + 1)
 *   delta_ext((on, sigma, n), e, x):
 *        sigma' = sigma - e                               if on  (time passed in this period)
 *        for each command c in the bag, in order:
 *            c = ON  and not on -> on := true,  sigma' := period
 *            c = OFF            -> on := false, sigma' := inf
 *   delta_con(s, x) = delta_ext(delta_int(s), 0, x)
 */
#ifndef GREENHOUSE_MODELS_HEATER_HPP
#define GREENHOUSE_MODELS_HEATER_HPP

#include "../prelude.hpp"

#include <limits>
#include <ostream>
#include <tuple>

#include <cadmium/modeling/message_bag.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../debug_log.hpp"
#include "../messages.hpp"

namespace greenhouse {

struct heater_config {
    double pulse_period_s = 1.0;  ///< one energy unit per period while ON
};

struct heater_defs {
    struct command : public cadmium::in_port<heater_command> {};
    struct energy_pulse : public cadmium::out_port<int> {};
};

template <typename TIME>
struct heater_state {
    bool on = false;
    TIME sigma = std::numeric_limits<TIME>::infinity();  ///< time until the next pulse
    int pulses = 0;                                      ///< total pulses emitted so far
};

template <typename TIME>
std::ostream& operator<<(std::ostream& os, const heater_state<TIME>& s) {
    return os << (s.on ? "ON" : "OFF") << " sigma=" << s.sigma << " pulses=" << s.pulses;
}

template <typename TIME>
class heater {
    using defs = heater_defs;

public:
    using state_type = heater_state<TIME>;
    state_type state;
    heater_config config;

    using input_ports = std::tuple<typename defs::command>;
    using output_ports = std::tuple<typename defs::energy_pulse>;

    heater() = default;
    explicit heater(const heater_config& cfg) : config(cfg) {}

    void internal_transition() {
        state.pulses += 1;
        state.sigma = TIME(config.pulse_period_s);
    }

    void external_transition(TIME e, typename cadmium::make_message_bags<input_ports>::type mbs) {
        if (state.on) state.sigma = state.sigma - e;  // part of the period has already elapsed
        for (const heater_command& c : cadmium::get_messages<typename defs::command>(mbs)) {
            if (c.setting == power::on && !state.on) {
                state.on = true;
                state.sigma = TIME(config.pulse_period_s);
            } else if (c.setting == power::off) {
                state.on = false;
                state.sigma = std::numeric_limits<TIME>::infinity();
            }
        }
        debug_note("heater: now ", state.on ? "ON" : "OFF");
    }

    void confluence_transition(TIME /*e*/, typename cadmium::make_message_bags<input_ports>::type mbs) {
        debug_note("heater: CONFLUENCE (command arrives exactly when a pulse is due)");
        internal_transition();
        external_transition(TIME{}, std::move(mbs));
    }

    typename cadmium::make_message_bags<output_ports>::type output() const {
        typename cadmium::make_message_bags<output_ports>::type bags;
        cadmium::get_messages<typename defs::energy_pulse>(bags).push_back(1);
        return bags;
    }

    TIME time_advance() const {
        return state.on ? state.sigma : std::numeric_limits<TIME>::infinity();
    }
};

}  // namespace greenhouse

#endif  // GREENHOUSE_MODELS_HEATER_HPP
