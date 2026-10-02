/**
 * @file messages.hpp
 * @brief Message (event value) types that travel between the greenhouse models.
 *
 * In DEVS, atomic models talk by sending *values* through *ports*. In Cadmium a port is a
 * C++ type that carries a `message_type`, and the compiler refuses to couple two ports whose
 * message types differ. This header defines the non-trivial message types of the project.
 * Simple messages (ints, floats) are used as-is.
 *
 * LOGGING NOTE
 *   Cadmium's loggers print messages with `operator<<` when one exists. A message type
 *   *without* `operator<<` is still legal, it is printed as
 *   "obscure message of type <name>" (`cadmium::logger::value_or_name`). The stock
 *   `reset_tick` message is an example of that second kind.
 */
#ifndef GREENHOUSE_MESSAGES_HPP
#define GREENHOUSE_MESSAGES_HPP

#include <ostream>

namespace greenhouse {

/// Heater power setting.
enum class power { off, on };

inline std::ostream& operator<<(std::ostream& os, power p) {
    return os << (p == power::on ? "ON" : "OFF");
}

/**
 * Command sent by the thermostat to the heater.
 * `because_of` records the temperature reading that triggered the command, purely so the
 * logs explain themselves.
 */
struct heater_command {
    power setting = power::off;
    float because_of = 0.0f;
};

inline bool operator==(const heater_command& a, const heater_command& b) {
    return a.setting == b.setting && a.because_of == b.because_of;
}

inline std::ostream& operator<<(std::ostream& os, const heater_command& c) {
    return os << "heater " << c.setting << " (reading " << c.because_of << ")";
}

}  // namespace greenhouse

#endif  // GREENHOUSE_MESSAGES_HPP
