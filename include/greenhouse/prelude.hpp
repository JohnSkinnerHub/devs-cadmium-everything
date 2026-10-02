/**
 * @file prelude.hpp
 * @brief Must be included BEFORE any Cadmium header (every header of this project does it).
 *
 * THE PROBLEM (found by experiment, not documented by Cadmium)
 *   `cadmium::dynamic::modeling::atomic::model_state_as_string()` prints a model's state with an
 *   UNQUALIFIED `oss << this->state`. For states that are `std::tuple`s (the stock accumulator's
 *   is `std::tuple<int, bool>`) the only printer is `cadmium::operator<<` from
 *   <cadmium/logger/tuple_to_ostream.hpp>. Because `std::tuple` lives in namespace `std`,
 *   argument-dependent lookup cannot find a `cadmium::` operator; the name must already be
 *   visible where `dynamic_atomic.hpp` DEFINES that member function. Therefore:
 *
 *       tuple_to_ostream.hpp must be included before any dynamic Cadmium header.
 *
 *   Get the order wrong and the failure is an unhelpful "no match for operator<<" deep inside a
 *   template instantiation, even though the same code compiles with the include order of the
 *   other file. Cadmium's own examples happen to include tuple_to_ostream.hpp first.
 *
 * THE SECOND HALF OF THE PROBLEM (and why including it "early" is not enough)
 *   Cell-DEVS identifies grid cells by `std::vector<int>` and prints them (model names such as
 *   "floor_(4,3)", log lines "(4,3) ; 21.5") with an unqualified `os << cell_id` inside
 *   namespace `cadmium::celldevs`. Its vector printer is a *global* function declared in
 *   <cadmium/celldevs/utils/utils.hpp>. Name lookup stops at the FIRST enclosing namespace that
 *   declares any `operator<<`: once `cadmium::operator<<` (the tuple printer above) exists,
 *   lookup from `cadmium::celldevs` stops in `cadmium` and never reaches the global vector
 *   printer, so the Cell-DEVS headers stop compiling.
 *   The two requirements pull in opposite directions:
 *       tuple states (dynamic atomics)   -> tuple printer must be declared FIRST
 *       vector ids (Cell-DEVS)           -> a vector printer must be visible from `cadmium::`
 *   The way out, without editing Cadmium: declare an equivalent vector printer INSIDE namespace
 *   `cadmium` right next to the tuple printer (below). Lookup from either `cadmium::dynamic` or
 *   `cadmium::celldevs` then finds both, and the output format is the same "(1,2)" as Cadmium's.
 *   (This only matters for programs that combine tuple-state models with Cell-DEVS, like demo 09.)
 *
 * THE THIRD TRAP (stock iestream_input is not self-contained)
 *   <cadmium/basic_model/pdevs/iestream.hpp> defines `operator<<(std::ostringstream&, ...)` but
 *   never includes <sstream>, so `std::ostringstream` is only forward-declared there and
 *   `os << "text"` has no match: the header fails to compile on its own, and compiles only if
 *   some earlier include happened to pull in <sstream>. We include it here so that it cannot
 *   depend on luck.
 *
 * THE FIX HERE
 *   Every header of this project starts with `#include "prelude.hpp"` (or the right relative
 *   path), which makes the rule impossible to forget.
 */
#ifndef GREENHOUSE_PRELUDE_HPP
#define GREENHOUSE_PRELUDE_HPP

#include <ostream>
#include <sstream>  // see "THE THIRD TRAP" above
#include <vector>

#include <cadmium/logger/tuple_to_ostream.hpp>

namespace cadmium {
/// Same format as Cadmium's own `operator<<(ostream&, vector<X>)` (global, in celldevs/utils): "(1,2,3)".
template <typename X>
std::ostream& operator<<(std::ostream& os, const std::vector<X>& values) {
    os << "(";
    const char* separator = "";
    for (const auto& value : values) {
        os << separator << value;
        separator = ",";
    }
    return os << ")";
}
}  // namespace cadmium

#endif  // GREENHOUSE_PRELUDE_HPP
