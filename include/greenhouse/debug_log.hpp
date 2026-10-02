/**
 * @file debug_log.hpp
 * @brief A user-level `logger_debug` channel that models can write to.
 *
 * WHY THIS EXISTS
 *   Cadmium declares a logger source called `logger_debug`, but none of its engines ever
 *   emits on it, it is reserved for *model authors*. The difficulty is that an atomic model
 *   is a plain class that does not know which LOGGER the runner was given, so it cannot
 *   write to "the" logger. The usual solution, used here, is a global logger of the
 *   `logger_debug` source that models call directly, and that the application can redirect
 *   (to a file, a string, or nowhere) at run time.
 *
 * HOW
 *   - `debug_logger` is a normal `cadmium::logger::logger<>` bound to `logger_debug`.
 *   - Its sink provider returns whatever stream was installed with `set_debug_target()`;
 *     by default that is a null stream, so debug logging costs almost nothing.
 *   - We re-use Cadmium's `run_info` event ("a free-text message") for the payload.
 *
 *   Thread-safety: the pointer swap is atomic, but a plain `std::ostringstream` is not safe
 *   for concurrent writers. When running with Cadmium's concurrent / OpenMP engines point
 *   the target at a `greenhouse::locked_stream` (see logging.hpp) instead.
 */
#ifndef GREENHOUSE_DEBUG_LOG_HPP
#define GREENHOUSE_DEBUG_LOG_HPP

#include "prelude.hpp"

#include <atomic>
#include <ostream>
#include <sstream>
#include <string>

#include <cadmium/logger/common_loggers.hpp>
#include <cadmium/logger/logger.hpp>

namespace greenhouse {

namespace detail {
/// A stream that discards everything (a streambuf whose overflow always "succeeds").
struct null_buffer : std::streambuf {
    int overflow(int c) override { return c; }
};
inline std::ostream& null_stream() {
    static null_buffer buffer;
    static std::ostream stream(&buffer);
    return stream;
}
inline std::atomic<std::ostream*>& debug_target() {
    static std::atomic<std::ostream*> target{nullptr};
    return target;
}
}  // namespace detail

/// Redirect model debug output. Pass nullptr to switch it off again.
inline void set_debug_target(std::ostream* os) { detail::debug_target().store(os); }

/// Sink provider contract (see `cadmium::logger::cout_sink_provider`): a static `sink()`.
struct debug_sink_provider {
    static std::ostream& sink() {
        std::ostream* os = detail::debug_target().load();
        return os ? *os : detail::null_stream();
    }
};

using debug_logger = cadmium::logger::logger<cadmium::logger::logger_debug,
                                             cadmium::logger::formatter<float>,
                                             debug_sink_provider>;

/// Convenience used by the models:  `debug_note("thermostat decided ", "ON");`
template <typename... Parts>
void debug_note(const Parts&... parts) {
    std::ostringstream oss;
    (oss << ... << parts);
    debug_logger::log<cadmium::logger::logger_debug, cadmium::logger::run_info>(oss.str());
}

}  // namespace greenhouse

#endif  // GREENHOUSE_DEBUG_LOG_HPP
