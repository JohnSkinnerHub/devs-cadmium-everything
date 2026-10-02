/**
 * @file cell_probe.hpp
 * @brief An adapter atomic model: turns one Cell-DEVS cell's published state into a plain reading.
 *
 * WHY IT IS NEEDED
 *   Cell-DEVS cells talk with `cell_state_message<id, state>` ("cell X now has state S").
 *   The thermostat wants a plain `float` temperature. Cell-DEVS lattices are *ordinary dynamic
 *   coupled models*, so they can be coupled to any other DEVS model, but the message types must
 *   match, and a small adapter model is the standard way to bridge two message vocabularies.
 *
 * WHAT IT DOES (a stateless-looking model with one pending output)
 *   X = { (cell_in, cell_state_message) }   Y = { (reading_out, float) }
 *   S = (pending: bool, reading: float)
 *   delta_ext: keep the LAST message from the watched cell; pending := true   (others are ignored)
 *   ta(s)    : 0 if pending else infinity          -> forwards immediately (zero-delay relay)
 *   lambda(s): { reading_out : reading }
 *   delta_int: pending := false
 *   delta_con: delta_int, then delta_ext
 */
#ifndef GREENHOUSE_FLOOR_CELL_PROBE_HPP
#define GREENHOUSE_FLOOR_CELL_PROBE_HPP

#include "../prelude.hpp"

#include <limits>
#include <ostream>
#include <tuple>

#include <cadmium/celldevs/cell/cell.hpp>
#include <cadmium/celldevs/utils/grid_utils.hpp>
#include <cadmium/modeling/message_bag.hpp>
#include <cadmium/modeling/ports.hpp>

namespace greenhouse {

/// The port a grid cell publishes on and listens to (identical types for every cell of the lattice).
using floor_cell_ports = cadmium::celldevs::cell_ports_def<cadmium::celldevs::cell_position, double>;

struct cell_probe_defs {
    struct cell_in : public floor_cell_ports::cell_in {};  // same message type as a cell's input
    struct reading_out : public cadmium::out_port<float> {};
};

struct probe_state {
    bool pending = false;
    float reading = 0.0f;
};

inline std::ostream& operator<<(std::ostream& os, const probe_state& s) {
    return os << (s.pending ? "pending " : "idle ") << s.reading;
}

template <typename TIME>
class cell_probe {
    using defs = cell_probe_defs;

public:
    using state_type = probe_state;
    state_type state;
    cadmium::celldevs::cell_position watched = {0, 0};

    using input_ports = std::tuple<typename defs::cell_in>;
    using output_ports = std::tuple<typename defs::reading_out>;

    cell_probe() = default;
    explicit cell_probe(const cadmium::celldevs::cell_position& cell) : watched(cell) {}

    void internal_transition() { state.pending = false; }

    void external_transition(TIME /*e*/, typename cadmium::make_message_bags<input_ports>::type mbs) {
        for (const auto& message : cadmium::get_messages<typename defs::cell_in>(mbs)) {
            if (message.cell_id == watched) {
                state.reading = static_cast<float>(message.state);
                state.pending = true;
            }
        }
    }

    void confluence_transition(TIME e, typename cadmium::make_message_bags<input_ports>::type mbs) {
        internal_transition();
        external_transition(e, std::move(mbs));
    }

    typename cadmium::make_message_bags<output_ports>::type output() const {
        typename cadmium::make_message_bags<output_ports>::type bags;
        cadmium::get_messages<typename defs::reading_out>(bags).push_back(state.reading);
        return bags;
    }

    TIME time_advance() const { return state.pending ? TIME{} : std::numeric_limits<TIME>::infinity(); }
};

}  // namespace greenhouse

#endif  // GREENHOUSE_FLOOR_CELL_PROBE_HPP
