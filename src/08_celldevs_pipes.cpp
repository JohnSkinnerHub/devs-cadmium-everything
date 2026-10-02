/**
 * @file 08_celldevs_pipes.cpp
 * @brief Cell-DEVS on a graph: the irrigation pipe network (string cell ids, JSON scenario).
 *
 * WHAT THIS DEMO SHOWS
 *   1. A single `cell` driven by hand: its state, its neighbour bookkeeping, its five DEVS
 *      functions, and what the delay buffer does to its output. (This is the part of Cadmium's
 *      Cell-DEVS that `grid_coupled` and `cells_coupled` automate for thousands of cells.)
 *   2. `cells_coupled`: build a network of cells with arbitrary ids from `data/pipes.json`
 *      (`add_cells_json`, `couple_cells`) and simulate it.
 *   3. Building a network in code (`add_cell`), and what the JSON machinery computes.
 *   4. The cell message type (`cell_state_message`) and the cell's port definitions.
 */
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

#include "greenhouse/check.hpp"
#include "greenhouse/fixed_time.hpp"
#include "greenhouse/floor/pipe_network.hpp"
#include "greenhouse/logging.hpp"
#include "greenhouse/trace.hpp"

#include <cadmium/celldevs/utils/grid_utils.hpp>  // cell_position (the grid id type) and the vector printer
#include <cadmium/engine/pdevs_dynamic_runner.hpp>

using namespace greenhouse;
namespace cd = cadmium::celldevs;
namespace lg = cadmium::logger;
using plain_cell = cd::cell<double, std::string, double, double>;
using ports = cd::cell_ports_def<std::string, double>;
using message = cd::cell_state_message<std::string, double>;
using in_bags = cadmium::make_message_bags<plain_cell::input_ports>::type;

/// A cell that mirrors neighbour "b" and publishes the new state after 2 time units.
class echo_cell : public plain_cell {
public:
    using plain_cell::plain_cell;  // inherit the (id, neighbourhood, state, delay) constructor
    double local_computation() const override { return state.neighbors_state.at("b"); }
    double output_delay(const double&) const override { return 2.0; }
};

/// The bag of messages a cell receives on its single input port.
static in_bags inbox(std::initializer_list<message> messages) {
    in_bags bags;
    for (const auto& m : messages) cadmium::get_messages<ports::cell_in>(bags).push_back(m);
    return bags;
}

/// The (cell id -> state) message a cell sends, read from an output() result.
static message published(const plain_cell& c) {
    auto out = c.output();
    return cadmium::get_messages<ports::cell_out>(out).front();
}

struct pipes_tag {};
template <typename TIME>
using trace_logger = lg::multilogger<
    lg::logger<lg::logger_global_time, trace_formatter<TIME>, memory_sink<pipes_tag>>,
    lg::logger<lg::logger_state, trace_formatter<TIME>, memory_sink<pipes_tag>>>;

template <typename Network>
void run(Network& network, double horizon) {
    memory_sink<pipes_tag>::clear();
    auto top = std::make_shared<Network>(network);  // models are shared: `network.snapshot()` still sees them
    cadmium::dynamic::engine::runner<double, trace_logger<double>> runner(top, 0.0);
    runner.run_until(horizon);
}

int main() {
    const double inf = std::numeric_limits<double>::infinity();
    using nb = std::unordered_map<std::string, double>;

    // =====================================================================================
    section("1. a single cell, driven by hand");
    // =====================================================================================
    {
        // a cell: id, neighbourhood {neighbour id -> vicinity}, initial state, delay buffer id
        plain_cell c("a", nb{{"b", 1.0}, {"c", 2.0}}, 5.0, "inertial");
        CHECK_EQ(c.cell_id, std::string("a"));
        CHECK_EQ(c.neighbors.size(), 2u);
        CHECK_EQ(c.state.current_state, 5.0);
        CHECK_EQ(c.state.neighbors_vicinity.at("c"), 2.0);
        CHECK_EQ(c.state.neighbors_state.at("b"), 0.0);  // neighbours' states start as S() until they speak
        CHECK_EQ(c.simulation_clock, 0.0);
        CHECK_THROWS((plain_cell()), std::invalid_argument);  // a cell cannot exist without its description

        // At t = 0 every cell announces its initial state: time_advance is 0 and output() says "a ; 5"
        CHECK_EQ(c.time_advance(), 0.0);
        const message hello = published(c);
        CHECK_EQ(hello.cell_id, std::string("a"));
        CHECK_EQ(hello.state, 5.0);
        std::ostringstream oss;
        oss << hello;  // cell_state_message prints as "<id> ; <state>"
        CHECK_EQ(oss.str(), std::string("a ; 5"));
        c.internal_transition();  // the announcement has been made; the buffer is empty again
        CHECK_EQ(c.time_advance(), inf);

        // The base class rule is "keep the state, never publish" (delay = infinity)
        CHECK_EQ(c.local_computation(), 5.0);
        CHECK_EQ(c.output_delay(5.0), inf);
        // A neighbour speaks (and a stranger too: its message is ignored)
        c.external_transition(1.0, inbox({message("b", 7.0), message("stranger", 99.0)}));
        CHECK_EQ(c.state.neighbors_state.at("b"), 7.0);
        CHECK(c.state.neighbors_state.count("stranger") == 0);
        CHECK_EQ(c.simulation_clock, 1.0);          // the cell tracks the global clock from the elapsed times
        CHECK_EQ(c.time_advance(), inf);            // the rule kept the state: nothing to publish
    }

    section("1b. a cell with a real rule: the delay buffer decides what gets published");
    {
        for (const std::string delay : {"inertial", "transport"}) {
            echo_cell a("a", nb{{"b", 1.0}}, 5.0, delay);
            a.internal_transition();                                   // initial announcement done
            a.external_transition(1.0, inbox({message("b", 7.0)}));   // t=1: new state 7, due at 1 + 2 = 3
            CHECK_EQ(a.state.current_state, 7.0);
            CHECK_EQ(a.time_advance(), 2.0);
            CHECK_EQ(published(a).state, 7.0);
            a.external_transition(0.5, inbox({message("b", 9.0)}));   // t=1.5: newer state 9, due at 3.5
            if (delay == "inertial") {
                // the newer decision CANCELS the pending 7: only 9 will ever be published
                CHECK_EQ(a.time_advance(), 2.0);
                CHECK_EQ(published(a).state, 9.0);
            } else {
                // transport keeps both: 7 is published at t=3 (in 1.5), then 9 at t=3.5 (0.5 later)
                CHECK_EQ(a.time_advance(), 1.5);
                CHECK_EQ(published(a).state, 7.0);
                a.internal_transition();
                CHECK_EQ(a.time_advance(), 0.5);
                CHECK_EQ(published(a).state, 9.0);
            }
            std::cout << "  (" << delay << ") ok\n";
        }
        // delta_con = delta_int then delta_ext: an input arriving exactly when the t=0 announcement is due
        echo_cell b("a", nb{{"b", 1.0}}, 5.0, "inertial");
        CHECK_EQ(b.time_advance(), 0.0);
        b.confluence_transition(0.0, inbox({message("b", 7.0)}));
        CHECK_EQ(b.state.current_state, 7.0);
        CHECK_EQ(b.time_advance(), 2.0);
    }

    // =====================================================================================
    section("2. the pipe network from data/pipes.json (cells_coupled)");
    // =====================================================================================
    {
        pipe_network<double> network("pipes");
        network.add_cells_json("data/pipes.json");  // one add_cell_json() call per cell of the file
        network.couple_cells();                     // one IC per (neighbour -> cell) pair
        CHECK_EQ(network._models.size(), 7u);       // tank, j1, j2, j3, head1, head2, head3
        CHECK(network.get_cell_name("j1") == "pipes_j1");
        run(network, 60.0);
        auto p = network.snapshot();
        std::cout << "  pressures at t=60:";
        for (const auto& kv : p) std::cout << " " << kv.first << "=" << kv.second;
        std::cout << "\n";
        CHECK_EQ(p.at("tank"), 10.0);                 // the source never changes
        CHECK(p.at("j1") < p.at("tank") && p.at("j1") > 0);
        CHECK(p.at("j2") < p.at("j1"));               // pressure drops downstream
        CHECK(p.at("j3") < p.at("j1"));
        CHECK(p.at("head1") < p.at("j2"));            // ... and sprinkler heads lose pressure
        CHECK(p.at("head3") < p.at("j3"));
        CHECK_EQ(p.at("head1"), p.at("head2"));       // symmetric branches agree
    }

    // =====================================================================================
    section("3. building the network in code, and the JSON machinery for graph cells");
    // =====================================================================================
    {
        pipe_network<double> small("small");
        // add_cell<CELL>(id, neighbourhood, constructor arguments...)
        small.add_cell<source_cell>("src", node_neighborhood{}, 10.0, "inertial");
        small.add_cell<node_cell>("sink", node_neighborhood{{"sink", 0.0}, {"src", 1.0}}, 0.0, "inertial", node_config{0.5, 0.0, 1.0});
        CHECK_EQ(small._models.size(), 2u);
        // two cells with the same id are refused
        CHECK_THROWS((small.add_cell<node_cell>("sink", node_neighborhood{}, 0.0, "inertial", node_config{})), std::bad_typeid);
        small.couple_cells();
        run(small, 30.0);
        auto p = small.snapshot();
        std::cout << "  sink pressure after 30 s: " << p.at("sink") << " (source: " << p.at("src") << ")\n";
        CHECK(p.at("sink") > 9.9);  // no loss: the sink converges to the source pressure
        // (it listens to itself, see pipe_network.hpp: without that self-loop it would stall at 5)

        // What add_cells_json computes from the file, step by step:
        pipe_network<double> probe("probe");
        probe.add_cells_json("data/pipes.json");
        cadmium::json file;
        std::ifstream("data/pipes.json") >> file;
        auto configs = probe.get_default_configs(file["cells"]);
        CHECK_EQ(configs.size(), 8u);  // default + 7 cells
        CHECK_EQ(configs.at("default").delay, std::string("transport"));
        CHECK_EQ(configs.at("tank").cell_type, std::string("source"));
        CHECK_EQ(configs.at("tank").state, 10.0);
        CHECK_EQ(configs.at("j1").neighborhood.size(), 4u);  // itself, tank, j2, j3
        CHECK_EQ(configs.at("j1").neighborhood.at("tank"), 1.0);
        auto head3 = configs.at("head3").config.get<node_config>();
        CHECK_EQ(head3.loss, 0.2);     // patched by the group
        CHECK_EQ(head3.latency, 2.0);  // patched by the group
        CHECK_EQ(head3.k, 0.3);        // inherited from "default" (merge-patch keeps untouched keys)
        // cell_config is the plain record behind every cell group: (delay, cell_type, state, neighbourhood, config)
        cd::cell_config<std::string, double, double> by_hand("hybrid", "node", 1.5, nb{{"x", 0.5}}, cadmium::json{{"k", 0.1}});
        CHECK_EQ(by_hand.delay, std::string("hybrid"));
        CHECK_EQ(by_hand.neighborhood.at("x"), 0.5);
        CHECK_EQ(by_hand.config.at("k").get<double>(), 0.1);
        cd::cell_config<std::string, double, double> blank;  // default-constructible too
        CHECK(blank.delay.empty());
        // the neighbourhood of a graph cell is a plain JSON object {id: vicinity}
        auto parsed = probe.parse_neighborhood(cadmium::json::parse(R"({"x": 0.5, "y": 1.5})"));
        CHECK_EQ(parsed.size(), 2u);
        CHECK_EQ(parsed.at("y"), 1.5);
        // the individual readers
        auto d = probe.read_default_cell_config(cadmium::json::parse(R"({"cell_type": "node", "state": 2.0})"));
        CHECK_EQ(d.cell_type, std::string("node"));
        CHECK_EQ(d.delay, std::string("inertial"));  // the built-in default delay
        auto derived = probe.read_cell_config(cadmium::json::parse(R"({"state": 4.0})"), configs.at("default"));
        CHECK_EQ(derived.state, 4.0);
        CHECK_EQ(derived.delay, std::string("transport"));
        // an unknown cell type is an error
        CHECK_THROWS(probe.add_cell_json("valve", "v1", node_neighborhood{}, 0.0, "inertial", cadmium::json()), std::out_of_range);
    }

    // =====================================================================================
    section("4. cell ports and messages");
    // =====================================================================================
    {
        static_assert(ports::cell_in::kind == cadmium::port_kind::in, "cell_in is an input port");
        static_assert(ports::cell_out::kind == cadmium::port_kind::out, "cell_out is an output port");
        static_assert(std::is_same<ports::cell_in::message_type, message>::value, "carries cell_state_message<C,S>");
        static_assert(std::is_same<plain_cell::input_ports, std::tuple<ports::cell_in>>::value, "a cell has one input port");
        static_assert(std::is_same<plain_cell::output_ports, std::tuple<ports::cell_out>>::value, "and one output port");
        CHECK_MSG(true, "cell_ports_def: cell_in / cell_out carry cell_state_message<C, S> (static_assert)");
        // the grid flavour uses std::vector<int> as the id; its messages print "(x,y) ; state"
        cd::cell_state_message<cd::cell_position, double> grid_message({3, 4}, 21.5);
        std::ostringstream oss;
        oss << grid_message;
        CHECK_EQ(oss.str(), std::string("(3,4) ; 21.5"));
        // and the vector printer is available on its own
        std::ostringstream positions;
        positions << cd::cell_position{1, 2, 3};
        CHECK_EQ(positions.str(), std::string("(1,2,3)"));
    }

    return check_summary("08_celldevs_pipes");
}
