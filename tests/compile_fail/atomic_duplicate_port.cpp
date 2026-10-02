// EXPECT: once
// The same port type listed twice in one model is ambiguous (the engines look ports up by type).
#include "common.hpp"
#ifdef BREAK
using inputs = std::tuple<in_int, in_int>;
#else
using inputs = std::tuple<in_int, other_in_int>;
#endif
template <typename TIME>
struct model : basic_model<TIME, inputs, std::tuple<out_int>> {};
int main() { cadmium::concept::pdevs::atomic_model_assert<model>(); }
