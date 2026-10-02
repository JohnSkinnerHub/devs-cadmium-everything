/**
 * @file floor_coupled.hpp
 * @brief The greenhouse floor as a Cell-DEVS *coupled model* built from a JSON scenario.
 *
 * `grid_coupled<T, S, V>` is Cadmium's lattice builder. It is a *dynamic* coupled model (a
 * lattice whose size is read from a file cannot be a template argument), and it does the tedious
 * part for us:
 *   - `add_lattice_json(file)` reads the shape, the wrapping flag, the default cell
 *     configuration and the cell groups from the scenario file, computes every cell's absolute
 *     neighbourhood (clipping at the borders unless the grid is wrapped) and asks us to create
 *     each cell through the virtual `add_grid_cell_json(...)`;
 *   - `couple_cells()` then creates one internal coupling for every (neighbour -> cell) pair.
 * The only thing we write is the *factory*: which C++ cell class implements which "cell_type"
 * string of the JSON file.
 *
 * Scenario file format (see data/floor.json):
 *   "shape":   lattice size, e.g. [9, 7]       "wrapped": true -> torus
 *   "cells":   groups of cell settings. "default" is the base; every other group overrides
 *              some fields:  delay (inertial|transport|hybrid), cell_type, state, neighborhood,
 *              config (merged key by key into the default config)
 *   "cell_map": which positions belong to which group
 */
#ifndef GREENHOUSE_FLOOR_FLOOR_COUPLED_HPP
#define GREENHOUSE_FLOOR_FLOOR_COUPLED_HPP

#include "../prelude.hpp"

#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include <cadmium/celldevs/coupled/grid_coupled.hpp>

#include "floor_cells.hpp"

namespace greenhouse {

template <typename T>
class floor_coupled : public cadmium::celldevs::grid_coupled<T, double, double> {
    using base = cadmium::celldevs::grid_coupled<T, double, double>;

public:
    explicit floor_coupled(const std::string& id) : base(id) {}

    /// The cell factory: called once per cell by `add_lattice_json`.
    void add_grid_cell_json(const std::string& cell_type, cell_map<double, double>& map, const std::string& delay_id,
                            const cadmium::json& config) override {
        if (cell_type == "air") {
            // grid_coupled::add_cell<CELL>(map, delay_id, extra constructor arguments...)
            this->template add_cell<air_cell>(map, delay_id, config.get<air_config>());
        } else if (cell_type == "heater" || cell_type == "window") {
            this->template add_cell<fixed_cell>(map, delay_id);
        } else {
            throw std::out_of_range("floor_coupled: unknown cell_type '" + cell_type + "'");
        }
    }

    /**
     * Build the lattice from a `grid_scenario` constructed IN CODE (no JSON file).
     *
     * This is exactly what `add_lattice_json` does after it has parsed the file, and what
     * Cadmium's own `grid_coupled::add_lattice()` is meant to do. That function cannot be used in
     * this Cadmium revision: it calls `scenario.get_states()`, which `grid_scenario` no longer
     * has (it is a template that was never instantiated, so the compiler never complained).
     * Hence this small replacement, which goes through the same cell factory.
     */
    void add_scenario(cadmium::celldevs::grid_scenario<double, double>& scenario) {
        for (const auto& entry : scenario.configs) {
            auto map = scenario.get_cell_map(entry.first);
            add_grid_cell_json(entry.second.cell_type, map, entry.second.delay, entry.second.config);
        }
    }

    /**
     * Publish one cell's output on the BOUNDARY of the whole lattice, so that other models can be
     * coupled to it from outside.
     *
     * A lattice built by `grid_coupled` has no external ports: cells only talk to each other. To
     * observe a cell from outside we (1) declare an external output port on the coupled model and
     * (2) add an EOC (external output coupling) from that cell to the port. Both lists are public
     * members of `dynamic::modeling::coupled`. The port type is the cell's own output port, so
     * the message type matches automatically.
     */
    void expose_cell(const cell_position& position) {
        using cell_out = cadmium::celldevs::cell_ports_def<cell_position, double>::cell_out;
        this->_output_ports.push_back(typeid(cell_out));
        this->_eoc.push_back(cadmium::dynamic::translate::make_EOC<cell_out, cell_out>(this->get_cell_name(position)));
    }

    /// Temperatures of every cell right now, read straight from the cell models.
    std::map<cell_position, double> snapshot() const {
        std::map<cell_position, double> temperatures;
        for (const auto& m : this->_models) {
            auto cell = std::dynamic_pointer_cast<cadmium::celldevs::grid_cell<T, double, double>>(m);
            if (cell) temperatures[cell->cell_id] = cell->state.current_state;
        }
        return temperatures;
    }

    /// A text heat map (x grows to the right, y downwards), one number per cell.
    std::string heatmap(int width, int height) const {
        auto temps = snapshot();
        std::ostringstream oss;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) oss << std::setw(6) << std::fixed << std::setprecision(1) << temps.at({x, y});
            oss << "\n";
        }
        return oss.str();
    }
};

}  // namespace greenhouse

#endif  // GREENHOUSE_FLOOR_FLOOR_COUPLED_HPP
