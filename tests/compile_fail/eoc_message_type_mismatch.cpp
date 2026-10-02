// EXPECT: The message type does not match in EOC description
// An external output coupling must connect ports with the same message type.
#include "common.hpp"
#ifdef BREAK
using external_out = out_text;  // the coupled model emits strings ...
#else
using external_out = out_int;
#endif
template <typename T>
using top = coupled_model<T, none, std::tuple<external_out>, models_tuple<producer>, none,
                          std::tuple<EOC<producer, out_int, external_out>>,   // ... but the producer emits ints
                          none>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
