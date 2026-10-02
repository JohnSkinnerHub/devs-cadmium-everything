// EXPECT: The port in a to_model in a IC is not an input port
// The destination end of an internal coupling must be an INPUT port of the destination model.
#include "common.hpp"
#ifdef BREAK
using to_port = out_int;    // an output port used where an input port is needed
#else
using to_port = in_int;
#endif
template <typename T>
using top = coupled_model<T, none, none, models_tuple<producer, consumer>, none, none,
                          std::tuple<IC<producer, out_int, consumer, to_port>>>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
