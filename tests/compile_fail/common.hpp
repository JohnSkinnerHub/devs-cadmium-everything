// Shared building blocks for the compile-fail snippets: three tiny, VALID atomic models.
//
//   producer<T>       no inputs,   one output  out_int  (int)
//   relay<T>          one input    in_int,     one output out_int
//   consumer<T>       one input    in_int,     no outputs
//   text_consumer<T>  one input    in_text (std::string), no outputs   (a model with a different message type)
//
// Each snippet builds a coupled model out of these and asks Cadmium to validate it. The line that
// makes it invalid is guarded by `#ifdef BREAK`.
#ifndef COMPILE_FAIL_COMMON_HPP
#define COMPILE_FAIL_COMMON_HPP

#include <limits>
#include <string>
#include <tuple>

#include <cadmium/concept/atomic_model_assert.hpp>
#include <cadmium/concept/coupled_model_assert.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/message_bag.hpp>
#include <cadmium/modeling/ports.hpp>

struct out_int : public cadmium::out_port<int> {};
struct other_out_int : public cadmium::out_port<int> {};
struct in_int : public cadmium::in_port<int> {};
struct other_in_int : public cadmium::in_port<int> {};
struct out_text : public cadmium::out_port<std::string> {};
struct in_text : public cadmium::in_port<std::string> {};

// A fully valid model with the given port lists. Snippets that need a broken model write their own.
template <typename TIME, typename IP, typename OP>
struct basic_model {
    using state_type = int;
    state_type state = 0;
    using input_ports = IP;
    using output_ports = OP;
    void internal_transition() {}
    void external_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
    void confluence_transition(TIME, typename cadmium::make_message_bags<input_ports>::type) {}
    typename cadmium::make_message_bags<output_ports>::type output() const { return {}; }
    TIME time_advance() const { return std::numeric_limits<TIME>::infinity(); }
};

template <typename TIME> struct producer : basic_model<TIME, std::tuple<>, std::tuple<out_int>> {};
template <typename TIME> struct relay : basic_model<TIME, std::tuple<in_int>, std::tuple<out_int>> {};
template <typename TIME> struct consumer : basic_model<TIME, std::tuple<in_int>, std::tuple<>> {};
template <typename TIME> struct text_consumer : basic_model<TIME, std::tuple<in_text>, std::tuple<>> {};

using cadmium::modeling::EIC;
using cadmium::modeling::EOC;
using cadmium::modeling::IC;
using cadmium::modeling::models_tuple;
using cadmium::modeling::pdevs::coupled_model;
using none = std::tuple<>;

#endif
