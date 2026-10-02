/**
 * @file floor_cells.hpp
 * @brief The cells of the greenhouse floor: a heat-diffusion Cell-DEVS model.
 *
 * WHAT CELL-DEVS IS
 *   Cell-DEVS (Wainer) describes spatial systems as a lattice of *cells*, each one a DEVS
 *   atomic model with the SAME local rule. A cell
 *     - keeps a state (here: a temperature),
 *     - knows its *neighbours* (and a "vicinity" weight for each),
 *     - listens to the states its neighbours publish,
 *     - applies its local rule to compute a new state, and
 *     - publishes the new state to its neighbours *after a delay*, but only if it CHANGED
 *       (so a settled region of the lattice generates no events at all).
 *   The delay is what makes Cell-DEVS richer than cellular automata: events happen at
 *   arbitrary real-valued times, and three delay semantics are offered
 *   (`inertial`, `transport`, `hybrid`, see delay_buffer/ in Cadmium and demo 07).
 *
 * THE PHYSICS WE MODEL (deliberately simple)
 *   The floor is a rectangular grid of air cells (state: temperature in degrees Celsius).
 *   Heat flows between neighbouring cells:
 *
 *       T' = T + diffusivity * SUM_n  vicinity_n * (T_n - T)           (rounded to 0.1 degree)
 *
 *   vicinity_n is the *conductance* between this cell and neighbour n: 1.0 for the four direct
 *   neighbours, 0.5 for the diagonals (set in the JSON scenario). Two special cell kinds
 *   never change: HEATERS (fixed hot temperature) and WINDOWS (fixed cold temperature).
 *   The rounding is essential: without it, temperatures would creep by ever smaller amounts
 *   forever and the simulation would never settle. With it, a cell stops publishing once its
 *   rounded temperature stops changing.
 *
 *   Publication delay of an air cell: `base_delay + delay_per_degree * |change|`,
 *   big temperature jumps take longer to "settle" (sensor lag). Because the delay depends
 *   on the new state, a cell can schedule a new output BEFORE an earlier one was delivered,
 *   the situation in which the three delay types differ.
 *
 * HOW IT IS WRITTEN IN CADMIUM
 *   - The state `S` is a `double`, the vicinity `V` is a `double`.
 *   - `air_cell` derives from `grid_cell<T, S, V>` and overrides ONLY two virtual functions:
 *       `local_computation()`   the rule: next state from current state + neighbours
 *       `output_delay(next)`    how long until the new state is published
 *     Everything else (the five DEVS functions, buffering, message handling) is inherited from
 *     `cell` / `grid_cell`.
 *   - Heaters and windows use `grid_cell` itself (its default rule is "keep my state").
 *   - Parameters come from JSON: `air_config` is built by `from_json`.
 */
#ifndef GREENHOUSE_FLOOR_FLOOR_CELLS_HPP
#define GREENHOUSE_FLOOR_FLOOR_CELLS_HPP

#include "../prelude.hpp"

#include <cmath>
#include <string>

#include <cadmium/celldevs/cell/grid_cell.hpp>
#include <cadmium/json/json.hpp>

namespace greenhouse {

using cadmium::celldevs::cell_map;
using cadmium::celldevs::cell_position;
using cadmium::celldevs::cell_unordered;

/// Per-cell parameters of the air cells, read from the "config" object of the scenario JSON.
struct air_config {
    double diffusivity = 0.2;        ///< fraction of the temperature gap exchanged per step
    double base_delay = 0.5;         ///< minimum time to publish a new temperature
    double delay_per_degree = 0.1;   ///< extra time per degree of change
};

/// Cadmium/nlohmann convention: a free `from_json` next to the type makes `json.get<air_config>()` work.
/// Missing keys keep their defaults, which is what lets a cell group override just one parameter.
inline void from_json(const cadmium::json& j, air_config& c) {
    if (j.contains("diffusivity")) j.at("diffusivity").get_to(c.diffusivity);
    if (j.contains("base_delay")) j.at("base_delay").get_to(c.base_delay);
    if (j.contains("delay_per_degree")) j.at("delay_per_degree").get_to(c.delay_per_degree);
}

inline double round_tenth(double x) { return std::round(x * 10.0) / 10.0; }

/// A cell of air: temperature diffuses to and from its neighbours.
template <typename T>
class air_cell : public cadmium::celldevs::grid_cell<T, double, double> {
    using base = cadmium::celldevs::grid_cell<T, double, double>;

public:
    using base::neighbors;  // the neighbour ids (cell_positions)
    using base::state;      // state.current_state, state.neighbors_state, state.neighbors_vicinity
    air_config config;

    /// Cadmium's concept checks default-construct models (unevaluated), so a default constructor
    /// must exist even though the engine never calls it.
    air_cell() : base() {}

    /// The constructor `grid_coupled::add_cell` expects: (position, neighbourhood, initial state,
    /// cell map, delay buffer id, extra user arguments...).
    air_cell(const cell_position& id, const cell_unordered<double>& neighborhood, double initial_state,
             const cell_map<double, double>& map, const std::string& delay_id, const air_config& cfg)
        : base(id, neighborhood, initial_state, map, delay_id), config(cfg) {}

    /// The local rule (the heart of the model). Called after every batch of neighbour updates.
    double local_computation() const override {
        const double own = state.current_state;
        double flow = 0.0;
        for (const auto& n : neighbors) {
            if (n == this->cell_id) continue;  // the neighbourhood includes the cell itself
            flow += state.neighbors_vicinity.at(n) * (state.neighbors_state.at(n) - own);
        }
        return round_tenth(own + config.diffusivity * flow);
    }

    /// How long until the new state is visible to the neighbours.
    T output_delay(const double& next_state) const override {
        return T(config.base_delay + config.delay_per_degree * std::fabs(next_state - state.current_state));
    }
};

/// Heaters and windows never change: the stock `grid_cell` already behaves that way (its default
/// `local_computation` returns the current state), so it is used as it is.
template <typename T>
using fixed_cell = cadmium::celldevs::grid_cell<T, double, double>;

}  // namespace greenhouse

#endif  // GREENHOUSE_FLOOR_FLOOR_CELLS_HPP
