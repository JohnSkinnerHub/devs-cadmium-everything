// EXPECT: Output function does not exist or does not return the right message bags
// output() must return the tuple of message bags built from output_ports.
#include "common.hpp"
template <typename TIME>
struct model {
    using state_type = int;
    state_type state = 0;
    using input_ports = std::tuple<in_int>;
    using output_ports = std::tuple<out_int>;
    void internal_transition() {}
    void external_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
    void confluence_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
#ifdef BREAK
    int output() const { return 0; }
#else
    typename cadmium::make_message_bags<output_ports>::type output() const { return {}; }
#endif
    TIME time_advance() const { return std::numeric_limits<TIME>::infinity(); }
};
int main() { cadmium::concept::pdevs::atomic_model_assert<model>(); }
