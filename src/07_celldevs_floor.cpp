/**
 * @file 07_celldevs_floor.cpp
 * @brief Cell-DEVS: the thermal floor of the greenhouse (a lattice of cells, from JSON).
 *
 * WHAT THIS DEMO SHOWS
 *   1. The grid toolbox (`grid_scenario`, `cell_map`): distances, wrapping, neighbourhoods.
 *   2. The three output delay buffers, driven directly, and the buffer factory.
 *   3. A full lattice simulation configured by `data/floor.json`: building it
 *      (`add_lattice_json`, `couple_cells`), running it on the dynamic engine, reading results.
 *   4. The neighbourhood types of the JSON format and the JSON default/merge machinery.
 *   5. Building a lattice WITHOUT JSON (`grid_scenario` + `add_lattice` / `add_cell`).
 *   6. Delay semantics matter: the same lattice with inertial / transport / hybrid delays.
 *
 * Cell-DEVS in one picture (the cell's life):
 *
 *   neighbours' states --(cell_in)--> [ delta_ext: store neighbour states,
 *                                       next := local_computation(),
 *                                       if next != state: schedule it after output_delay(next) ]
 *   timer expires      --(delta_int)-> [ publish the scheduled state on cell_out ]
 */
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

#include "greenhouse/check.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/floor/floor_coupled.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/trace.hpp"

#include <cadmium/celldevs/delay_buffer/delay_buffer_factory.hpp>
#include <cadmium/engine/pdevs_dynamic_runner.hpp>

using namespace greenhouse;
namespace cd = cadmium::celldevs;
namespace lg = cadmium::logger;
using scenario_t = cd::grid_scenario<double, double>;
using config_t = cd::grid_cell_config<double, double>;

struct floor_trace_tag {};
template <typename TIME>
using trace_logger = lg::multilogger<
    lg::logger<lg::logger_global_time, trace_formatter<TIME>, memory_sink<floor_trace_tag>>,
    lg::logger<lg::logger_state, trace_formatter<TIME>, memory_sink<floor_trace_tag>>>;

/// Writes a copy of data/floor.json with a different delay buffer (and optionally a smaller grid),
/// using the JSON type Cadmium re-exports (cadmium::json = nlohmann::json).
static std::string write_floor_variant(const std::string& delay, bool small, const std::string& name) {
    cadmium::json j;
    {
        std::ifstream in("data/floor.json");
        in >> j;
    }
    j["cells"]["default"]["delay"] = delay;
    if (small) {  // a 5x3 floor: window on the left, heater on the right, sensor in the middle (2,1)
        j["shape"] = {5, 3};
        j["cell_map"] = cadmium::json::parse(R"({"window": [[0, 1]], "heater": [[4, 1]]})");
    }
    std::system("mkdir -p build/out");
    const std::string path = "build/out/" + name + ".json";
    std::ofstream(path) << j.dump(2);
    return path;
}

struct run_result {
    std::map<cell_position, double> final_temperatures;
    std::vector<trace_event> trace;
    double time_to_reach(const std::string& cell_name, double threshold) const {
        for (const auto& e : trace)
            if (e.kind == 'S' && e.model == cell_name && std::atof(e.text.c_str()) >= threshold) return e.time;
        return -1;
    }
};

template <typename TIME>
run_result simulate_floor(const std::string& json_path, double horizon) {
    floor_coupled<TIME> floor("floor");
    floor.add_lattice_json(json_path);  // reads shape, groups, neighbourhoods; calls add_grid_cell_json per cell
    floor.couple_cells();               // one IC per (neighbour -> cell)
    memory_sink<floor_trace_tag>::clear();
    auto top = std::make_shared<floor_coupled<TIME>>(floor);  // the engine wants a shared_ptr (models are shared)
    cadmium::dynamic::engine::runner<TIME, trace_logger<TIME>> runner(top, TIME(0.0));
    runner.run_until(TIME(horizon));
    return {floor.snapshot(), parse_trace(memory_sink<floor_trace_tag>::contents())};
}

int main() {
    const double inf = std::numeric_limits<double>::infinity();

    // =====================================================================================
    section("1. the grid toolbox: grid_scenario static functions");
    // =====================================================================================
    {
        const cell_position shape{5, 4};  // 5 columns (x), 4 rows (y)
        CHECK(scenario_t::cell_in_scenario({4, 3}, shape));
        CHECK(!scenario_t::cell_in_scenario({5, 0}, shape));
        CHECK(!scenario_t::cell_in_scenario({-1, 0}, shape));

        // next_cell enumerates the lattice, x first
        CHECK((scenario_t::next_cell({0, 0}, shape, 0) == cell_position{1, 0}));
        CHECK((scenario_t::next_cell({4, 0}, shape, 0) == cell_position{0, 1}));
        CHECK_THROWS(scenario_t::next_cell({4, 3}, shape, 0), std::overflow_error);  // last cell reached

        // distance vectors: plain and on a torus ("wrapped": the shortest way round)
        CHECK((scenario_t::distance_vector({0, 0}, {4, 3}, shape, false) == cell_position{4, 3}));
        CHECK((scenario_t::distance_vector({0, 0}, {4, 3}, shape, true) == cell_position{-1, -1}));
        // destination_cell is the inverse: origin + distance, wrapping or refusing to leave the grid
        CHECK((scenario_t::destination_cell({4, 3}, {1, 1}, shape, true) == cell_position{0, 0}));
        CHECK_THROWS(scenario_t::destination_cell({4, 3}, {1, 1}, shape, false), std::overflow_error);

        // four distance metrics
        CHECK_EQ(scenario_t::manhattan_distance({0, 0}, {4, 3}, shape, false), 7);
        CHECK_EQ(scenario_t::manhattan_distance({0, 0}, {4, 3}, shape, true), 2);
        CHECK_EQ(scenario_t::chebyshev_distance({0, 0}, {4, 3}, shape, false), 4);
        CHECK_EQ(scenario_t::chebyshev_distance({0, 0}, {4, 3}, shape, true), 1);
        CHECK_NEAR(scenario_t::euclidean_distance({0, 0}, {4, 3}, shape, false), 5.0, 1e-9);
        CHECK_NEAR(scenario_t::euclidean_distance({0, 0}, {4, 3}, shape, true), std::sqrt(2.0), 1e-9);
        CHECK_NEAR(scenario_t::n_norm_distance({0, 0}, {4, 3}, 3, shape, false), std::cbrt(64.0 + 27.0), 1e-5);

        // neighbourhoods around the origin, in any number of dimensions
        CHECK_EQ(scenario_t::moore_neighborhood(2, 1).size(), 9u);        // the 3x3 block
        CHECK_EQ(scenario_t::von_neumann_neighborhood(2, 1).size(), 5u);  // the plus sign
        CHECK_EQ(scenario_t::moore_neighborhood(2, 2).size(), 25u);
        CHECK_EQ(scenario_t::von_neumann_neighborhood(2, 2).size(), 13u);
        CHECK_EQ(scenario_t::moore_neighborhood(3, 1).size(), 27u);
        CHECK_EQ(scenario_t::von_neumann_neighborhood(3, 1).size(), 7u);
        // "biassed" variants are the same sets expressed in 0-based block coordinates; `unbias`
        // re-centres them on the origin (this is how the two public ones are built)
        auto biassed = scenario_t::biassed_moore_neighborhood(2, 1);
        CHECK_EQ(biassed.size(), 9u);
        CHECK((biassed.front() == cell_position{0, 0} && biassed.back() == cell_position{2, 2}));
        scenario_t::unbias_neighborhood(biassed, {1, 1});
        CHECK((biassed.front() == cell_position{-1, -1} && biassed.back() == cell_position{1, 1}));
        CHECK_EQ(scenario_t::biassed_von_neumann_neighborhood(2, 1).size(), 5u);
    }

    section("1b. grid_scenario instances and cell_map");
    {
        cd::cell_unordered<double> von_neumann;  // relative offsets -> vicinity
        for (const auto& offset : scenario_t::von_neumann_neighborhood(2, 1)) von_neumann[offset] = 1.0;
        config_t config("inertial", "air", 20.0, von_neumann, cadmium::json());
        scenario_t scenario({5, 4}, config, /*wrapped=*/false);  // every cell gets `config`
        CHECK_EQ(scenario.dimension, 2u);
        CHECK_EQ(scenario.configs.size(), 20u);
        CHECK(!scenario.wrapped);

        // one cell can be given a different configuration
        config_t hot("transport", "heater", 99.0, von_neumann, cadmium::json());
        scenario.set_initial_config({2, 2}, hot);
        CHECK_EQ(scenario.configs.at({2, 2}).state, 99.0);
        CHECK_EQ(scenario.configs.at({1, 1}).state, 20.0);
        scenario.set_initial_config(config);  // ... and the whole lattice can be reset
        CHECK_EQ(scenario.configs.at({2, 2}).state, 20.0);

        // the same geometry functions, bound to the scenario's own shape and wrapping
        CHECK_EQ(scenario.manhattan_distance({0, 0}, {4, 3}), 7);
        CHECK_EQ(scenario.chebyshev_distance({0, 0}, {4, 3}), 4);
        CHECK_NEAR(scenario.euclidean_distance({0, 0}, {4, 3}), 5.0, 1e-9);
        CHECK_NEAR(scenario.n_norm_distance({0, 0}, {4, 3}, 1), 7.0, 1e-9);
        CHECK((scenario.distance_vector({0, 0}, {4, 3}) == cell_position{4, 3}));
        CHECK((scenario.destination_cell({1, 1}, {1, 1}) == cell_position{2, 2}));
        CHECK(scenario.cell_in_scenario({4, 3}));
        CHECK((scenario.next_cell({1, 0}, 0) == cell_position{2, 0}));

        // get_cell_map: the cell's absolute neighbourhood, clipped at the lattice border
        cd::cell_map<double, double> corner = scenario.get_cell_map({0, 0});
        CHECK_EQ(corner.neighborhood.size(), 3u);  // itself, (1,0), (0,1): the other two are off-grid
        cd::cell_map<double, double> middle = scenario.get_cell_map({2, 2});
        CHECK_EQ(middle.neighborhood.size(), 5u);
        CHECK_EQ(middle.state, 20.0);
        CHECK(!middle.wrapped);
        CHECK((middle.shape == cell_position{5, 4}));
        CHECK((middle.location == cell_position{2, 2}));

        // cell_map's own helpers measure from the cell's location
        CHECK_EQ(middle.manhattan_distance({4, 3}), 3);
        CHECK_EQ(middle.chebyshev_distance({4, 3}), 2);
        CHECK_NEAR(middle.euclidean_distance({4, 3}), std::sqrt(5.0), 1e-9);
        CHECK_NEAR(middle.n_norm_distance({4, 3}, 1), 3.0, 1e-9);
        CHECK((middle.neighbor({1, 0}) == cell_position{3, 2}));   // relative -> absolute
        CHECK((middle.relative({3, 2}) == cell_position{1, 0}));   // absolute -> relative
        CHECK_THROWS((cd::cell_map<double, double>()), std::exception);  // no default construction

        // on a torus the corner has all five neighbours
        scenario_t torus({5, 4}, config, /*wrapped=*/true);
        CHECK_EQ(torus.get_cell_map({0, 0}).neighborhood.size(), 5u);
    }

    // =====================================================================================
    section("2. delay buffers: how a cell schedules the publication of its new states");
    // =====================================================================================
    {
        // The abstract base: every operation is a no-op / default.
        cd::delay_buffer<double, double> base;
        CHECK_EQ(base.next_timeout(), inf);
        CHECK_EQ(base.next_state(), 0.0);
        base.add_to_buffer(1.0, 1.0);
        base.pop_buffer();

        // Same schedule for the three types: a state (10) due at t=2, a LATER computed state (20) due
        // EARLIER at t=1, and finally a state (30) due at t=3. Which of them are ever published?
        cd::inertial_delay_buffer<double, double> inertial;
        cd::transport_delay_buffer<double, double> transport;
        cd::hybrid_delay_buffer<double, double> hybrid;
        CHECK_EQ(inertial.next_timeout(), inf);
        CHECK_EQ(transport.next_timeout(), inf);
        CHECK_EQ(hybrid.next_timeout(), inf);
        for (cd::delay_buffer<double, double>* b : {static_cast<cd::delay_buffer<double, double>*>(&inertial),
                                                    static_cast<cd::delay_buffer<double, double>*>(&transport),
                                                    static_cast<cd::delay_buffer<double, double>*>(&hybrid)}) {
            b->add_to_buffer(10.0, 2.0);
            b->add_to_buffer(20.0, 1.0);
            b->add_to_buffer(30.0, 3.0);
        }
        // INERTIAL: only the latest scheduling survives ("a new decision cancels the pending one")
        CHECK_EQ(inertial.next_timeout(), 3.0);
        CHECK_EQ(inertial.next_state(), 30.0);
        inertial.pop_buffer();
        CHECK_EQ(inertial.next_timeout(), inf);
        CHECK_EQ(inertial.next_state(), 30.0);  // still reports the last published state

        // TRANSPORT: everything is published, in order of scheduled time
        CHECK_EQ(transport.next_timeout(), 1.0);
        CHECK_EQ(transport.next_state(), 20.0);
        transport.pop_buffer();
        CHECK_EQ(transport.next_timeout(), 2.0);
        CHECK_EQ(transport.next_state(), 10.0);
        transport.pop_buffer();
        CHECK_EQ(transport.next_state(), 30.0);
        transport.pop_buffer();
        CHECK_EQ(transport.next_timeout(), inf);
        CHECK_EQ(transport.next_state(), 30.0);  // last published
        transport.pop_buffer();                  // popping an empty buffer is harmless
        transport.add_to_buffer(1.0, 5.0);
        transport.add_to_buffer(2.0, 5.0);  // same scheduled time: the later state replaces the earlier
        CHECK_EQ(transport.next_state(), 2.0);

        // HYBRID: a new scheduling cancels the pending ones scheduled at or AFTER its own time,
        // earlier ones are kept ("transport for the past, inertial for the future")
        CHECK_EQ(hybrid.next_timeout(), 1.0);
        CHECK_EQ(hybrid.next_state(), 20.0);  // the 10 at t=2 was cancelled by the 20 at t=1
        hybrid.pop_buffer();
        CHECK_EQ(hybrid.next_timeout(), 3.0);
        CHECK_EQ(hybrid.next_state(), 30.0);
        hybrid.add_to_buffer(5.0, 3.0);  // replaces the pending 30 (same time)
        CHECK_EQ(hybrid.next_state(), 5.0);
        hybrid.pop_buffer();
        CHECK_EQ(hybrid.next_timeout(), inf);
        CHECK_EQ(hybrid.next_state(), 5.0);
        hybrid.pop_buffer();

        // the factory turns the JSON "delay" string into a buffer
        using factory = cd::delay_buffer_factory<double, double>;
        auto made_inertial = factory::create_delay_buffer("inertial");
        auto made_transport = factory::create_delay_buffer("transport");
        auto made_hybrid = factory::create_delay_buffer("hybrid");
        CHECK(dynamic_cast<cd::inertial_delay_buffer<double, double>*>(made_inertial.get()) != nullptr);
        CHECK(dynamic_cast<cd::transport_delay_buffer<double, double>*>(made_transport.get()) != nullptr);
        CHECK(dynamic_cast<cd::hybrid_delay_buffer<double, double>*>(made_hybrid.get()) != nullptr);
        CHECK_THROWS(factory::create_delay_buffer("quantum"), std::out_of_range);
    }

    // =====================================================================================
    section("3. the thermal floor from data/floor.json");
    // =====================================================================================
    {
        auto result = simulate_floor<double>("data/floor.json", 60.0);
        std::cout << "  temperatures at t=60 (x right, y down; window column x=0, heater at (7,3)):\n";
        {
            std::ostringstream map;
            for (int y = 0; y < 7; ++y) {
                map << "    ";
                for (int x = 0; x < 9; ++x) map << std::setw(6) << std::fixed << std::setprecision(1) << result.final_temperatures.at({x, y});
                map << "\n";
            }
            std::cout << map.str();
        }
        CHECK_EQ(result.final_temperatures.size(), 63u);  // 9 x 7 cells
        // fixed cells never change
        for (int y : {2, 3, 4}) CHECK_EQ((result.final_temperatures.at({0, y})), 5.0);
        CHECK_EQ((result.final_temperatures.at({7, 3})), 40.0);
        // everything else lies between the coldest and the hottest source
        bool in_range = true;
        for (const auto& kv : result.final_temperatures) in_range = in_range && kv.second >= 5.0 && kv.second <= 40.0;
        CHECK(in_range);
        // along the middle row, temperature rises from the window towards the heater
        bool rising = true;
        for (int x = 0; x < 7; ++x) rising = rising && result.final_temperatures.at({x, 3}) < result.final_temperatures.at({x + 1, 3});
        CHECK(rising);
        // the sensor cell (4,3) started at 15 and warmed up
        const double sensor = result.final_temperatures.at({4, 3});
        std::cout << "  sensor cell (4,3): 15.0 -> " << sensor << "; crossed 18 degrees at t="
                  << result.time_to_reach("floor_(4,3)", 18.0) << "\n";
        CHECK(sensor > 20.0 && sensor < 23.0);
        CHECK(result.time_to_reach("floor_(4,3)", 18.0) > 0);
        // the "insulated" group (diffusivity 0.05) reacts more slowly than ordinary cells: compare
        // the top-row cell (4,0) (insulated) with the equally-placed bottom-row cell (4,6)
        CHECK(result.time_to_reach("floor_(4,6)", 17.0) < result.time_to_reach("floor_(4,0)", 17.0));
        std::cout << "  an ordinary air cell (4,6) reached 17 at t=" << result.time_to_reach("floor_(4,6)", 17.0)
                  << ", the insulated cell (4,0) at t=" << result.time_to_reach("floor_(4,0)", 17.0) << "\n";
    }

    // =====================================================================================
    section("4. the JSON format: neighbourhood types, defaults and merging");
    // =====================================================================================
    {
        floor_coupled<double> floor("probe");
        floor.add_lattice_json("data/floor.json");  // sets the shape that parse_neighborhood needs
        auto parse = [&floor](const char* text) { return floor.parse_neighborhood(cadmium::json::parse(text)); };

        CHECK_EQ(parse(R"([{"type":"von_neumann","range":1,"vicinity":1.0}])").size(), 5u);
        CHECK_EQ(parse(R"([{"type":"von_neumann","range":2,"vicinity":1.0}])").size(), 13u);
        CHECK_EQ(parse(R"([{"type":"moore","range":1,"vicinity":1.0}])").size(), 9u);
        CHECK_EQ(parse(R"([{"type":"moore","vicinity":1.0}])").size(), 9u);  // range defaults to 1
        auto custom = parse(R"([{"type":"relative","neighbors":[[1,0],[-1,0]],"vicinity":0.25}])");
        CHECK_EQ(custom.size(), 2u);
        CHECK_EQ(custom.at({1, 0}), 0.25);
        // several entries are merged; a later entry overrides the vicinity of a repeated offset
        auto merged = parse(R"([{"type":"von_neumann","vicinity":1.0},{"type":"relative","neighbors":[[0,0]],"vicinity":9.0}])");
        CHECK_EQ(merged.size(), 5u);
        CHECK_EQ(merged.at({0, 0}), 9.0);
        // "custom" is the deprecated spelling of "relative": it works but warns on stderr
        {
            std::ostringstream err;
            auto* old = std::cerr.rdbuf(err.rdbuf());
            auto old_style = parse(R"([{"type":"custom","neighbors":[[1,1]],"vicinity":1.0}])");
            std::cerr.rdbuf(old);
            CHECK_EQ(old_style.size(), 1u);
            CHECK(err.str().find("Deprecation warning") != std::string::npos);
        }
        // absolute / remove are announced but not implemented; unknown types are rejected
        CHECK_THROWS(parse(R"([{"type":"absolute","neighbors":[[1,1]],"vicinity":1.0}])"), std::logic_error);
        CHECK_THROWS(parse(R"([{"type":"remove","neighbors":[[1,1]],"vicinity":1.0}])"), std::logic_error);
        CHECK_THROWS(parse(R"([{"type":"hexagonal","vicinity":1.0}])"), std::bad_typeid);

        // The defaults/merge machinery: every group is the "default" group patched with its own fields.
        cadmium::json file;
        std::ifstream("data/floor.json") >> file;
        auto configs = floor.get_default_configs(file["cells"]);
        CHECK_EQ(configs.size(), 4u);  // default, window, heater, insulated
        CHECK_EQ(configs.at("default").delay, std::string("inertial"));
        CHECK_EQ(configs.at("default").cell_type, std::string("air"));
        CHECK_EQ(configs.at("default").state, 15.0);
        CHECK_EQ(configs.at("default").neighborhood.size(), 9u);  // 5 von Neumann + 4 diagonals
        CHECK_EQ(configs.at("window").state, 5.0);
        CHECK_EQ(configs.at("window").cell_type, std::string("window"));
        CHECK_EQ(configs.at("window").neighborhood.size(), 9u);   // inherited from default
        // "insulated" only patches diffusivity: the other config keys are inherited (merge-patch)
        auto insulated = configs.at("insulated").config.get<air_config>();
        CHECK_EQ(insulated.diffusivity, 0.05);
        CHECK_EQ(insulated.base_delay, 0.5);
        CHECK_EQ(insulated.delay_per_degree, 0.1);
        // the individual readers are public too
        auto only_defaults = floor.read_default_cell_config(cadmium::json::parse(R"({"state": 3.0})"));
        CHECK_EQ(only_defaults.delay, std::string("inertial"));     // built-in default
        CHECK_EQ(only_defaults.cell_type, std::string("default"));  // built-in default
        CHECK_EQ(only_defaults.state, 3.0);
        auto derived = floor.read_cell_config(cadmium::json::parse(R"({"delay": "hybrid"})"), configs.at("default"));
        CHECK_EQ(derived.delay, std::string("hybrid"));
        CHECK_EQ(derived.state, 15.0);  // everything else from the default
        // patch_default_item: apply a JSON merge patch to a default and convert the result
        CHECK_EQ(floor.patch_default_item<double>(cadmium::json(1.5), cadmium::json(2.5)), 2.5);

        // a cell type the factory does not know is an error
        scenario_t one_cell({1, 1}, config_t("inertial", "air", 0.0, {{{0, 0}, 1.0}}, cadmium::json()), false);
        auto map = one_cell.get_cell_map({0, 0});
        CHECK_THROWS(floor.add_grid_cell_json("plasma", map, "inertial", cadmium::json()), std::out_of_range);
    }

    // =====================================================================================
    section("5. building a lattice without JSON: grid_scenario + add_scenario / add_cell");
    // =====================================================================================
    {
        cd::cell_unordered<double> neighbours;
        for (const auto& offset : scenario_t::von_neumann_neighborhood(2, 1)) neighbours[offset] = 1.0;
        // the per-cell "config" is JSON, exactly as it would be in the file
        const cadmium::json air_json = {{"diffusivity", 0.25}};
        config_t air_config_("inertial", "air", 10.0, neighbours, air_json);
        scenario_t scenario({4, 3}, air_config_, /*wrapped=*/false);
        scenario.set_initial_config({0, 1}, config_t("inertial", "window", 0.0, neighbours, cadmium::json()));
        scenario.set_initial_config({3, 1}, config_t("inertial", "heater", 30.0, neighbours, cadmium::json()));

        floor_coupled<double> floor("handbuilt");
        // One cell per scenario entry, created by the same factory the JSON route uses.
        // (Cadmium's own grid_coupled::add_lattice would do this but does not compile, see README.)
        floor.add_scenario(scenario);
        CHECK_EQ(floor._models.size(), 12u);
        // ... and add_cell<CELL>(cell_map, delay, args...) adds one more, explicitly
        // a cell_map can also be built by hand: (lattice shape, location, state, absolute neighbourhood, wrapped)
        cd::cell_map<double, double> extra_map({4, 3}, {9, 9}, 0.0, {{{9, 9}, 1.0}}, false);
        floor.add_cell<fixed_cell>(extra_map, "transport");
        CHECK_EQ(floor._models.size(), 13u);
        CHECK(floor.get_cell_name({9, 9}) == "handbuilt_(9,9)");  // "<coupled id>_<position>"
        // adding a cell with a position already present is refused
        auto dup_map = scenario.get_cell_map({1, 1});
        CHECK_THROWS(floor.add_cell<fixed_cell>(dup_map, "inertial"), std::bad_typeid);
        std::cout << "  cell model names look like: " << floor.get_cell_name({2, 1}) << "\n";
    }

    // =====================================================================================
    section("6. delay semantics change the dynamics (5x3 floor, window | sensor | heater)");
    // =====================================================================================
    {
        struct outcome { std::string delay; run_result result; };
        std::vector<outcome> outcomes;
        for (const std::string d : {"inertial", "transport", "hybrid"})
            outcomes.push_back({d, simulate_floor<double>(write_floor_variant(d, true, "floor_small_" + d), 30.0)});
        std::cout << "  delay       events   sensor(2,1) reaches 20   final sensor\n";
        for (const auto& o : outcomes)
            std::cout << "  " << std::left << std::setw(10) << o.delay << std::right << std::setw(8) << o.result.trace.size()
                      << std::setw(14) << o.result.time_to_reach("floor_(2,1)", 20.0) << std::setw(18)
                      << o.result.final_temperatures.at({2, 1}) << "\n";
        const auto& inertial = outcomes[0].result;
        const auto& transport = outcomes[1].result;
        const auto& hybrid = outcomes[2].result;
        // inertial buffers drop superseded outputs: heat spreads more slowly and with far fewer events
        CHECK(inertial.time_to_reach("floor_(2,1)", 20.0) > 1.5 * transport.time_to_reach("floor_(2,1)", 20.0));
        CHECK(inertial.trace.size() < hybrid.trace.size());
        // hybrid cancels pending outputs that a newer one supersedes: fewer events than transport
        CHECK(hybrid.trace.size() < transport.trace.size());
        CHECK_NEAR(transport.time_to_reach("floor_(2,1)", 20.0), hybrid.time_to_reach("floor_(2,1)", 20.0), 0.5);
        // ... but all three settle to (nearly) the same equilibrium: delays shape transients, not steady states
        for (const auto& kv : transport.final_temperatures) {
            CHECK_NEAR(kv.second, inertial.final_temperatures.at(kv.first), 0.6);
            CHECK_NEAR(kv.second, hybrid.final_temperatures.at(kv.first), 0.2);
        }

        // the lattice also runs with the custom integer-millisecond time type (transport buffers
        // need std::hash<TIME>, which fixed_time provides)
        auto fixed = simulate_floor<fixed_time>(write_floor_variant("transport", true, "floor_small_fixed"), 30.0);
        for (const auto& kv : transport.final_temperatures) CHECK_NEAR(kv.second, fixed.final_temperatures.at(kv.first), 0.6);
        std::cout << "  (fixed_time run: same equilibrium, " << fixed.trace.size() << " trace events)\n";
    }

    return check_summary("07_celldevs_floor");
}
