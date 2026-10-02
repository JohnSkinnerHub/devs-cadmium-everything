// EXPECT: Output function does not exist or does not return the right message bags
// Classic DEVS models communicate through message BOXES (at most one message per port).
#include "common.hpp"
#include <cadmium/modeling/message_box.hpp>
template <typename TIME>
struct model {
    using state_type = int;
    state_type state = 0;
    using input_ports = std::tuple<in_int>;
    using output_ports = std::tuple<out_int>;
    void internal_transition() {}
    void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type) {}
#ifdef BREAK
    typename cadmium::make_message_bags<output_ports>::type output() const { return {}; }   // a bag, not a box
#else
    typename cadmium::make_message_box<output_ports>::type output() const { return {}; }
#endif
    TIME time_advance() const { return std::numeric_limits<TIME>::infinity(); }
};
int main() { cadmium::concept::devs::atomic_model_assert<model>(); }
