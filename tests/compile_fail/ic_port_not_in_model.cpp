// EXPECT: Output port used in IC is not defined in the submodel
// The port named in a coupling must really be one of the model's ports.
#include "common.hpp"
#ifdef BREAK
using from_port = other_out_int;   // an out_port<int>, but the producer does not have it
#else
using from_port = out_int;
#endif
template <typename T>
using top = coupled_model<T, none, none, models_tuple<producer, consumer>, none, none,
                          std::tuple<IC<producer, from_port, consumer, in_int>>>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
