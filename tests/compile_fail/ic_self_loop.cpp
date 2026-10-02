// EXPECT: The IC detected a coupling-to-self loop
// A sub-model may not be coupled to itself (it would feed on its own output at the same instant).
#include "common.hpp"
#ifdef BREAK
template <typename T> using sink_model = relay<T>;   // relay -> relay: a self loop
using sink_port = in_int;
#else
template <typename T> using sink_model = consumer<T>;
using sink_port = in_int;
#endif
template <typename T>
using top = coupled_model<T, none, none, models_tuple<relay, sink_model>, none, none,
                          std::tuple<IC<relay, out_int, sink_model, sink_port>>>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
