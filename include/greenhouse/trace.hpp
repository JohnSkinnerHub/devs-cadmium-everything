/**
 * @file trace.hpp
 * @brief Reads back the text produced by `trace_formatter` so demos and tests can ASSERT on a
 *        simulation instead of eyeballing it.
 *
 * A "trace" is just the sequence of lines the loggers wrote. Parsing it gives a list of
 * `trace_event`s: (time, model, text). From that, helpers answer questions like
 * "which values did the energy meter report, and when?".
 */
#ifndef GREENHOUSE_TRACE_HPP
#define GREENHOUSE_TRACE_HPP

#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace greenhouse {

struct trace_event {
    double time = 0;     ///< global time when the line was logged
    char kind = '?';     ///< 'O' = output (OUT), 'S' = state (ST)
    std::string model;   ///< model id with namespaces and template arguments stripped, e.g. "heater"
    std::string text;    ///< the rest of the line
};

/// "cadmium::basic_models::pdevs::accumulator<int, float>" -> "accumulator";
/// "greenhouse::water_meter<double>" -> "water_meter"; ids that are not types are kept as they are.
inline std::string short_model_name(std::string id) {
    auto lt = id.find('<');
    if (lt != std::string::npos) id = id.substr(0, lt);
    auto colons = id.rfind("::");
    if (colons != std::string::npos) id = id.substr(colons + 2);
    return id;
}

/// Parse `trace_formatter` output (tab separated lines: T / OUT / ST).
inline std::vector<trace_event> parse_trace(const std::string& text) {
    std::vector<trace_event> events;
    std::istringstream in(text);
    std::string line;
    double now = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::size_t start = 0, tab;
        while ((tab = line.find('\t', start)) != std::string::npos) {
            cols.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        cols.push_back(line.substr(start));
        if (cols[0] == "T" && cols.size() >= 2) {
            now = (cols[1] == "inf") ? 1e300 : std::atof(cols[1].c_str());
        } else if ((cols[0] == "OUT" || cols[0] == "ST") && cols.size() >= 3) {
            trace_event e;
            e.time = now;
            e.kind = cols[0] == "OUT" ? 'O' : 'S';
            e.model = short_model_name(cols[1]);
            e.text = cols[2];
            events.push_back(std::move(e));
        }
    }
    return events;
}

/// True if an OUT text such as "[port: {}, other: {1}]" carries at least one message.
inline bool has_messages(const std::string& out_text) {
    for (std::size_t i = 0; i + 1 < out_text.size(); ++i)
        if (out_text[i] == '{' && out_text[i + 1] != '}') return true;
    return false;
}

/// All non-empty outputs of `model` as (time, text) pairs, in simulation order.
inline std::vector<std::pair<double, std::string>> outputs_of(const std::vector<trace_event>& trace,
                                                              const std::string& model) {
    std::vector<std::pair<double, std::string>> result;
    for (const auto& e : trace)
        if (e.kind == 'O' && e.model == model && has_messages(e.text)) result.emplace_back(e.time, e.text);
    return result;
}

/// The first integer found inside "{...}" of an output text: "[...sum: {5}]" -> 5.
inline int first_int_in_braces(const std::string& text) {
    auto open = text.find('{');
    return open == std::string::npos ? 0 : std::atoi(text.c_str() + open + 1);
}

/// Every value an accumulator-like model reported, with the time it did so: (5, 2), (10, 0), ...
inline std::vector<std::pair<double, int>> reports_of(const std::vector<trace_event>& trace,
                                                      const std::string& model) {
    std::vector<std::pair<double, int>> result;
    for (const auto& e : outputs_of(trace, model)) result.emplace_back(e.first, first_int_in_braces(e.second));
    return result;
}

/// Last logged state of `model` (empty string if it never logged one).
inline std::string last_state_of(const std::vector<trace_event>& trace, const std::string& model) {
    std::string last;
    for (const auto& e : trace)
        if (e.kind == 'S' && e.model == model) last = e.text;
    return last;
}

}  // namespace greenhouse

#endif  // GREENHOUSE_TRACE_HPP
