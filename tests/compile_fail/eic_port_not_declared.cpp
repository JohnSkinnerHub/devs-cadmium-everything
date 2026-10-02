// EXPECT: External port in EIC is not defined as external port in the model
// The external end of an EIC must be declared in the coupled model's own input port list.
#include "common.hpp"
#ifdef BREAK
using declared = std::tuple<other_in_int>;   // the model declares one input port ...
#else
using declared = std::tuple<in_int>;
#endif
template <typename T>
using top = coupled_model<T, declared, none, models_tuple<consumer>,
                          std::tuple<EIC<in_int, consumer, in_int>>,                // ... and couples a different one
                          none, none>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
