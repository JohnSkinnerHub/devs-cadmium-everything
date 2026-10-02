// EXPECT: Time advance function does not exist or does not return the right type of time
// time_advance() must return the model's TIME type.
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
    typename cadmium::make_message_bags<output_ports>::type output() const { return {}; }
#ifdef BREAK
    int time_advance() const { return 1; }   // an int, not a TIME
#else
    TIME time_advance() const { return 1; }
#endif
};
int main() { cadmium::concept::pdevs::atomic_model_assert<model>(); }
