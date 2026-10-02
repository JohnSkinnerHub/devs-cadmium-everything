// EXPECT: TIME datatype has no infinity defined
// run_until_passivate() runs until the next event time is infinity, so TIME must have one.
#include "common.hpp"
#include <cadmium/engine/pdevs_runner.hpp>
#include <cadmium/logger/common_loggers.hpp>
#ifdef BREAK
using time_type = int;      // std::numeric_limits<int>::has_infinity == false
#else
using time_type = float;
#endif
template <typename T>
using top = coupled_model<T, none, none, models_tuple<producer, consumer>, none, none,
                          std::tuple<IC<producer, out_int, consumer, in_int>>>;
int main() {
    cadmium::engine::runner<time_type, top, cadmium::logger::not_logger> runner{time_type(0)};
    runner.run_until_passivate();
}
