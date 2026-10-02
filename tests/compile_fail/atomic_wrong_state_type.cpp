// EXPECT: state is undefined or has the wrong type
// `state` must be a member of the declared `state_type`.
#include "common.hpp"
template <typename TIME>
struct model {
    using state_type = int;
#ifdef BREAK
    double state = 0;     // declared state_type is int, the member is a double
#else
    int state = 0;
#endif
    using input_ports = std::tuple<in_int>;
    using output_ports = std::tuple<out_int>;
    void internal_transition() {}
    void external_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
    void confluence_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
    typename cadmium::make_message_bags<output_ports>::type output() const { return {}; }
    TIME time_advance() const { return std::numeric_limits<TIME>::infinity(); }
};
int main() { cadmium::concept::pdevs::atomic_model_assert<model>(); }
