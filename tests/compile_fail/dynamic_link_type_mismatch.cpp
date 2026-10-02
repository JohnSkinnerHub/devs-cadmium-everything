// EXPECT: PORT_FROM message type and PORT_TO message types must be the same type
// The dynamic engine erases port types at run time, but every `link` still checks, at compile
// time, that the two ports carry the same message type.
#include <cadmium/logger/tuple_to_ostream.hpp>
#include "common.hpp"
#include <cadmium/engine/pdevs_dynamic_link.hpp>
#ifdef BREAK
using destination = in_text;
#else
using destination = in_int;
#endif
int main() { cadmium::dynamic::engine::link<out_int, destination> l; (void)l; }
