// EXPECT: confluence_transition
// A Parallel DEVS atomic model must define delta_con (confluence_transition).
#include "common.hpp"
template <typename TIME>
struct model {
    using state_type = int;
    state_type state = 0;
    using input_ports = std::tuple<in_int>;
    using output_ports = std::tuple<out_int>;
    void internal_transition() {}
    void external_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
#ifndef BREAK
    void confluence_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
#endif
    typename cadmium::make_message_bags<output_ports>::type output() const { return {}; }
    TIME time_advance() const { return std::numeric_limits<TIME>::infinity(); }
};
int main() { cadmium::concept::pdevs::atomic_model_assert<model>(); }
