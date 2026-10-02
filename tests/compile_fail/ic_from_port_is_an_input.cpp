// EXPECT: The port in from_model in a IC is not an output port
// The source end of an internal coupling must be an OUTPUT port of the source model.
#include "common.hpp"
#ifdef BREAK
using from_port = in_int;   // an input port used where an output port is needed
#else
using from_port = out_int;
#endif
template <typename T>
using top = coupled_model<T, none, none, models_tuple<producer, consumer>, none, none,
                          std::tuple<IC<producer, from_port, consumer, in_int>>>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
