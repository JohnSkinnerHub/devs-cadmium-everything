/**
 * @file pipe_network.hpp
 * @brief The irrigation pipe network: Cell-DEVS on a GRAPH instead of a grid.
 *
 * WHY THIS EXISTS
 *   The thermal floor (floor_cells.hpp) is a *lattice*: cell ids are coordinates and the
 *   neighbourhood follows from geometry. Cell-DEVS is more general: a cell can be any node with
 *   any id type and ANY neighbourhood. `cells_coupled<T, C, S, V>` is Cadmium's builder for that
 *   general case (`grid_coupled` derives from it). Here the ids are strings ("tank", "j1", ...).
 *
 * THE MODEL
 *   Water pressure in a tree of pipes:
 *
 *        tank --- j1 --+-- j2 --+-- head1        (heads are sprinklers: pressure leaks away)
 *                      |        +-- head2
 *                      +-- j3 ----- head3
 *
 *   state S    = pressure (double)
 *   vicinity V = pipe conductance towards that neighbour (double)
 *   rule       p' = p + k * SUM_n w_n * (p_n - p) - loss * p
 *   The tank is a SOURCE: constant pressure, it never changes (the stock `cell` already does
 *   exactly that, its default rule is "keep the state").
 *
 *   "Neighbourhood" in Cell-DEVS means "the cells I LISTEN to", so it is directed: the graph is
 *   undirected here only because we list each connection on both ends.
 *
 *   IMPORTANT PATTERN, a cell lists ITSELF as a neighbour (weight irrelevant, 0 here).
 *   A Cell-DEVS cell only recomputes when a message ARRIVES. When a cell publishes a new state,
 *   nobody it listens to has said anything new, so without the self-loop it would never look at
 *   its inputs again and the relaxation would stall after the first update. Listening to
 *   itself makes the cell hear its own (delayed) publication, which triggers the next
 *   local computation, and so on until the state stops changing. (The grid floor gets the same
 *   effect from the centre cell of its von Neumann neighbourhood.)
 *
 * SCENARIO FILE FORMAT (data/pipes.json), different from the grid format:
 *   "cells": { "default": {...}, "<cell id>": { cell_type, delay, state, neighborhood, config } }
 *   The neighbourhood of a graph cell is a plain object {"neighbour id": vicinity, ...}.
 *   Every non-"default" key IS a cell (grid files instead list positions in "cell_map").
 */
#ifndef GREENHOUSE_FLOOR_PIPE_NETWORK_HPP
#define GREENHOUSE_FLOOR_PIPE_NETWORK_HPP

#include "../prelude.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <cadmium/celldevs/cell/cell.hpp>
#include <cadmium/celldevs/coupled/cells_coupled.hpp>
#include <cadmium/json/json.hpp>

namespace greenhouse {

struct node_config {
    double k = 0.3;        ///< fraction of the pressure gap equalised per step
    double loss = 0.0;     ///< fraction of the pressure lost per step (open sprinkler heads)
    double latency = 1.0;  ///< time to publish a new pressure
};

inline void from_json(const cadmium::json& j, node_config& c) {
    if (j.contains("k")) j.at("k").get_to(c.k);
    if (j.contains("loss")) j.at("loss").get_to(c.loss);
    if (j.contains("latency")) j.at("latency").get_to(c.latency);
}

using node_id = std::string;
using node_neighborhood = std::unordered_map<node_id, double>;

/// A pipe junction or sprinkler head.
template <typename T>
class node_cell : public cadmium::celldevs::cell<T, node_id, double, double> {
    using base = cadmium::celldevs::cell<T, node_id, double, double>;

public:
    using base::neighbors;
    using base::state;
    node_config config;

    node_cell() : base() {}
    node_cell(const node_id& id, const node_neighborhood& neighborhood, double initial_pressure,
              const std::string& delay_id, const node_config& cfg)
        : base(id, neighborhood, initial_pressure, delay_id), config(cfg) {}

    double local_computation() const override {
        const double p = state.current_state;
        double inflow = 0.0;
        for (const auto& n : neighbors) {
            if (n == this->cell_id) continue;  // listening to ourselves is only a "wake me up" device, see below
            inflow += state.neighbors_vicinity.at(n) * (state.neighbors_state.at(n) - p);
        }
        return std::round((p + config.k * inflow - config.loss * p) * 100.0) / 100.0;  // hundredths settle
    }
    T output_delay(const double&) const override { return T(config.latency); }
};

/// The tank: the stock cell with its default (do-nothing) rule.
template <typename T>
using source_cell = cadmium::celldevs::cell<T, node_id, double, double>;

/// Builds the network from data/pipes.json.
template <typename T>
class pipe_network : public cadmium::celldevs::cells_coupled<T, node_id, double, double> {
    using base = cadmium::celldevs::cells_coupled<T, node_id, double, double>;

public:
    explicit pipe_network(const std::string& id) : base(id) {}

    /// The cell factory called by `add_cells_json` for every cell of the file.
    void add_cell_json(const std::string& cell_type, const node_id& cell_id, const node_neighborhood& neighborhood,
                       double initial_state, const std::string& delay_id, const cadmium::json& config) override {
        if (cell_type == "node") {
            this->template add_cell<node_cell>(cell_id, neighborhood, initial_state, delay_id, config.get<node_config>());
        } else if (cell_type == "source") {
            this->template add_cell<source_cell>(cell_id, neighborhood, initial_state, delay_id);
        } else {
            throw std::out_of_range("pipe_network: unknown cell_type '" + cell_type + "'");
        }
    }

    /// Pressure of every node, read straight from the cell models.
    std::map<node_id, double> snapshot() const {
        std::map<node_id, double> pressures;
        for (const auto& m : this->_models) {
            auto cell = std::dynamic_pointer_cast<cadmium::celldevs::cell<T, node_id, double, double>>(m);
            if (cell) pressures[cell->cell_id] = cell->state.current_state;
        }
        return pressures;
    }
};

}  // namespace greenhouse

#endif  // GREENHOUSE_FLOOR_PIPE_NETWORK_HPP
