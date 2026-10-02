// EXPECT: The message type does not match in IC description
// An internal coupling must connect ports that carry the SAME message type.
#include "common.hpp"
#ifdef BREAK
template <typename T> using target = text_consumer<T>;   // expects std::string ...
using target_port = in_text;                             // ... but the producer sends int
#else
template <typename T> using target = consumer<T>;
using target_port = in_int;
#endif
template <typename T>
using top = coupled_model<T, none, none, models_tuple<producer, target>, none, none,
                          std::tuple<IC<producer, out_int, target, target_port>>>;
int main() { cadmium::concept::pdevs::coupled_model_assert<top>(); }
