/**
 * @file stock_aliases.hpp
 * @brief Domain names for Cadmium's *stock* (ready-made) atomic models.
 *
 * WHY ALIASES ARE NEEDED
 *   A Cadmium *static* coupled model lists its sub-models as a list of **template templates**
 *   (`models_tuple<A, B, C>` where each is `template<typename TIME> class`). Stock models such
 *   as `accumulator<VALUE, TIME>` take an extra parameter, so they must be adapted to the
 *   one-parameter shape. Another subtlety: couplings and engines identify a sub-model by its
 *   *C++ type*, so two sub-models of the same coupled model must be two different types.
 *   The greenhouse needs two accumulators (water and energy), hence the two small subclasses.
 *
 * THE STOCK MODELS USED (all in namespace cadmium::basic_models::pdevs)
 *   int_generator_one_sec   emits the int 1 every second                  -> "irrigation pulse"
 *   reset_generator_five_sec emits a reset_tick every five seconds         -> "report tick"
 *   accumulator<int,TIME>   sums `add` messages, emits the sum on `reset`  -> the two meters
 *   filter_first_output     emits one `1` after its first input, then never again -> alarm latch
 *   passive<int,TIME>       absorbs messages forever and never outputs      -> alarm panel (sink)
 *   iestream_input<MSG,TIME> replays "time value" lines from a file          -> sensor feed
 *   generator<VALUE,TIME>   abstract periodic generator (see ticker in the classic demo)
 */
#ifndef GREENHOUSE_MODELS_STOCK_ALIASES_HPP
#define GREENHOUSE_MODELS_STOCK_ALIASES_HPP

#include "../prelude.hpp"  // tuple_to_ostream.hpp must precede every dynamic Cadmium header

#include <string>

#include <cadmium/basic_model/pdevs/accumulator.hpp>
#include <cadmium/basic_model/pdevs/filter_first_output.hpp>
#include <cadmium/basic_model/pdevs/generator.hpp>
#include <cadmium/basic_model/pdevs/iestream.hpp>
#include <cadmium/basic_model/pdevs/int_generator_one_sec.hpp>
#include <cadmium/basic_model/pdevs/passive.hpp>
#include <cadmium/basic_model/pdevs/reset_generator_five_sec.hpp>

namespace greenhouse {

namespace stock = cadmium::basic_models::pdevs;

/// Every second: one irrigation pulse.
template <typename TIME>
using pulse_clock = stock::int_generator_one_sec<TIME>;

/// Every five seconds: tell the meters to report and reset.
template <typename TIME>
using report_clock = stock::reset_generator_five_sec<TIME>;

/// Port definitions of the stock clocks (each has a single output port called `out`).
using pulse_clock_defs = stock::int_generator_one_sec_defs;
using report_clock_defs = stock::reset_generator_five_sec_defs;

/// Port/message definitions of the integer accumulators (add, reset, sum, reset_tick).
using meter_defs = stock::accumulator_defs<int>;
using report_tick_msg = meter_defs::reset_tick;

/// Counts irrigation pulses between two reports.
template <typename TIME>
struct water_meter : public stock::accumulator<int, TIME> {};

/// Counts heater energy units between two reports.
template <typename TIME>
struct energy_meter : public stock::accumulator<int, TIME> {};

/// Emits a single `1` the first time it receives anything (a one-shot alarm latch).
template <typename TIME>
using alarm_latch = stock::filter_first_output<TIME>;
using alarm_latch_defs = stock::filter_first_output_defs;

/// Absorbs the alarm for good (a model with no output and an infinite time advance).
template <typename TIME>
using alarm_panel = stock::passive<int, TIME>;
using alarm_panel_defs = stock::passive_defs<int>;

/// Where `sensor_feed()` looks for its file when it is default-constructed. Static engines
/// default-construct every model, so the path has to come from somewhere global.
inline std::string& sensor_feed_path() {
    static std::string path = "data/sensor_feed.txt";
    return path;
}

/**
 * Replays temperature readings from a text file: one "<time> <value>" pair per line.
 *
 * IMPORTANT quirk of the stock `iestream_input`/`Parser` (found by experiment): it detects the
 * end of the file with `file.eof()` *before* reading, so the data file must NOT end with a
 * newline, otherwise a failed extraction leaves an uninitialised time and a bogus event can
 * appear. `data/sensor_feed.txt` is therefore written without a trailing newline.
 */
template <typename TIME>
class sensor_feed : public stock::iestream_input<float, TIME> {
public:
    sensor_feed() : stock::iestream_input<float, TIME>(sensor_feed_path().c_str()) {}
    explicit sensor_feed(const std::string& path) : stock::iestream_input<float, TIME>(path.c_str()) {}
};

using sensor_feed_defs = stock::iestream_input_defs<float>;

}  // namespace greenhouse

#endif  // GREENHOUSE_MODELS_STOCK_ALIASES_HPP
