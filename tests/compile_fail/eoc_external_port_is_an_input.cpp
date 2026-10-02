// EXPECT: The external port in a EOC is not an output port
// The external end of an EOC must be an OUTPUT port of the coupled model.
#include "common.hpp"
#ifdef BREAK
using external = other_in_int;   // an input port where an output port is required
#else
using external = other_out_int;
#endif
template <typename T>
using top = coupled_model<T, none, std::tuple<external>, models_tuple<producer>, none,
                          std::tuple<EOC<producer, out_int, external>>, none>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
