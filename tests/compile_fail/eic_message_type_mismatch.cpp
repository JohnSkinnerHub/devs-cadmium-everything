// EXPECT: The message type does not match in EIC description
// An external input coupling must connect ports with the same message type.
#include "common.hpp"
#ifdef BREAK
using external_in = in_text;   // the coupled model receives strings ...
#else
using external_in = in_int;
#endif
template <typename T>
using top = coupled_model<T, std::tuple<external_in>, none, models_tuple<consumer>,
                          std::tuple<EIC<external_in, consumer, in_int>>,   // ... but the consumer wants ints
                          none, none>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
