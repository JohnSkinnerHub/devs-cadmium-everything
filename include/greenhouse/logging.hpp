/**
 * @file logging.hpp
 * @brief Custom Cadmium *sinks* and *formatters* used by the demos and the tests.
 *
 * HOW CADMIUM LOGGING IS ORGANISED (the mental model)
 *
 *     engine ----(source, event, args...)---> logger<SOURCE, FORMATTER, SINK> ---> text in a stream
 *
 *   - A **source** says *what kind* of information it is (`logger_state`, `logger_messages`,
 *     `logger_message_routing`, `logger_info`, `logger_debug`, `logger_global_time`,
 *     `logger_local_time`). A `logger<>` only reacts to ONE source; everything else is a no-op
 *     that the compiler removes (`if constexpr`).
 *   - An **event** says *which moment* of the simulation algorithm produced it
 *     (`sim_state`, `sim_messages_collect`, `coor_routing_collect_ic`, `run_global_time`, ...).
 *   - A **formatter** is a class with one static function per event; it turns the event's
 *     arguments into text. Cadmium ships `formatter<TIME>` (static engine) and
 *     `dynamic::logger::formatter<TIME>` (dynamic engine), the argument lists differ slightly.
 *   - A **sink provider** is a class with `static std::ostream& sink()`. Cadmium ships
 *     `cout_sink_provider` and `cerr_sink_provider`.
 *   - `multilogger<L1, L2, ...>` forwards every call to all of its loggers, that is how a
 *     run sends states to one file, messages to another, and so on.
 *
 *   The three custom pieces below plug into that design:
 *     `locked_stream`       a thread-safe in-memory stream (safe with the concurrent engines)
 *     `memory_sink<Tag>`    a line-atomic in-memory sink provider we can read back after the run
 *     `file_sink<Tag>`      a sink provider writing to a file
 *     `trace_formatter<T>`  a formatter producing one machine-readable line per interesting
 *                           event, understood by both the static and the dynamic engine
 */
#ifndef GREENHOUSE_LOGGING_HPP
#define GREENHOUSE_LOGGING_HPP

#include "prelude.hpp"

#include <fstream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>

#include <cadmium/logger/common_loggers.hpp>
#include <cadmium/logger/dynamic_common_loggers.hpp>
#include <cadmium/logger/logger.hpp>

namespace greenhouse {

// -----------------------------------------------------------------------------------------
// locked_stream: a std::ostream that many threads can insert into at the same time.
//
// Why a custom buffer: a std::stringbuf has a *put area*, and the inline fast path of
// `sputc` writes into it directly without calling any virtual function, so a lock inside an
// overridden `overflow()`/`xsputn()` is simply bypassed (an earlier version of this class lost
// more than half of the characters in a stress test). An *unbuffered* streambuf has no put
// area: every character takes the virtual path, where we hold the lock.
// Limits: concurrent insertion is safe; changing formatting flags (setw, hex, ...) from several
// threads at once is not, because those live in the ostream, not in the buffer.
// -----------------------------------------------------------------------------------------
class locked_stream : public std::ostream {
    class buffer : public std::streambuf {
    public:
        std::string snapshot() {
            std::lock_guard<std::mutex> lock(m_);
            return data_;
        }
        void reset() {
            std::lock_guard<std::mutex> lock(m_);
            data_.clear();
        }

    protected:
        int_type overflow(int_type c) override {
            if (!traits_type::eq_int_type(c, traits_type::eof())) {
                std::lock_guard<std::mutex> lock(m_);
                data_.push_back(traits_type::to_char_type(c));
            }
            return traits_type::not_eof(c);
        }
        std::streamsize xsputn(const char* s, std::streamsize n) override {
            std::lock_guard<std::mutex> lock(m_);
            data_.append(s, static_cast<std::size_t>(n));
            return n;
        }

    private:
        std::mutex m_;
        std::string data_;
    };
    buffer buf_;

public:
    locked_stream() : std::ostream(nullptr) { rdbuf(&buf_); }
    std::string str() { return buf_.snapshot(); }
    void clear_contents() { buf_.reset(); }
};

/**
 * Sink provider that collects log text in memory (one store per `Tag`) and is safe to use from
 * the concurrent and OpenMP engines.
 *
 * Why not just a `locked_stream`? Cadmium's logger writes one log line with TWO stream
 * operations (`sink() << text; sink() << std::endl;`). With several threads logging, the two
 * halves of different lines can be interleaved even if each operation is individually locked,
 * which would corrupt any later parsing. Here `sink()` hands every thread its OWN stream whose
 * buffer holds the partial line privately and publishes it to the shared store only when the
 * newline arrives, under a lock. Lines from different threads can therefore appear in any
 * order, but are never torn.
 *
 * Distinct tags = distinct stores, so one run can send states and messages to different places.
 */
template <typename Tag>
struct memory_sink {
    struct store_t {
        std::mutex m;
        std::string text;
    };
    static store_t& store() {
        static store_t s;
        return s;
    }

    class line_buffer : public std::streambuf {
        std::string pending_;

    protected:
        int_type overflow(int_type c) override {
            if (!traits_type::eq_int_type(c, traits_type::eof())) {
                pending_.push_back(traits_type::to_char_type(c));
                if (c == '\n') publish();
            }
            return traits_type::not_eof(c);
        }
        std::streamsize xsputn(const char* s, std::streamsize n) override {
            for (std::streamsize i = 0; i < n; ++i) overflow(traits_type::to_int_type(s[i]));
            return n;
        }

    private:
        void publish() {
            std::lock_guard<std::mutex> lock(store().m);
            store().text += pending_;
            pending_.clear();
        }
    };

    /// The function Cadmium requires of a sink provider. One stream per thread (see above).
    static std::ostream& sink() {
        thread_local line_buffer buffer;
        thread_local std::ostream stream(&buffer);
        return stream;
    }
    static std::string contents() {
        std::lock_guard<std::mutex> lock(store().m);
        return store().text;
    }
    static void clear() {
        std::lock_guard<std::mutex> lock(store().m);
        store().text.clear();
    }
};

/// Sink provider that writes into a file (one file per `Tag`). Call `open()` before running.
template <typename Tag>
struct file_sink {
    static std::ofstream& file() {
        static std::ofstream f;
        return f;
    }
    static void open(const std::string& path) {
        if (file().is_open()) file().close();
        file().open(path, std::ios::out | std::ios::trunc);
    }
    static void close() { if (file().is_open()) file().close(); }
    static std::ostream& sink() { return file(); }
};

// -----------------------------------------------------------------------------------------
// trace_formatter: one tab-separated line per event of interest.
//
//   T   <time>                         the global time reached by the runner
//   OUT <model> <port: {msgs}, ...>    what a model's output function produced (lambda)
//   ST  <model> <state>                a model's state after a transition
//
// A formatter only has to define the events of the sources it is attached to. This one is
// meant for `logger_global_time`, `logger_messages` and `logger_state`. The two engines call
// `sim_messages_collect` and `sim_state` with different argument lists, so both are
// overloaded here (static: no time argument; dynamic: time first).
// -----------------------------------------------------------------------------------------
template <typename TIME>
struct trace_formatter {
    // logger_global_time / run_global_time(t): the logger streams the returned value as-is
    static std::string run_global_time(const TIME& t) {
        std::ostringstream oss;
        oss << "T\t" << t;
        return oss.str();
    }
    // logger_messages / sim_messages_collect : static engine
    static std::string sim_messages_collect(const std::string& messages_by_port, const std::string& model_id) {
        return "OUT\t" + model_id + "\t" + messages_by_port;
    }
    // logger_messages / sim_messages_collect : dynamic engine
    static std::string sim_messages_collect(const TIME&, const std::string& model_id, const std::string& outbox) {
        return "OUT\t" + model_id + "\t" + outbox;
    }
    // logger_state / sim_state : static engine
    static std::string sim_state(const std::string& state, const std::string& model_id) {
        return "ST\t" + model_id + "\t" + state;
    }
    // logger_state / sim_state : dynamic engine
    static std::string sim_state(const TIME&, const std::string& model_id, const std::string& state) {
        return "ST\t" + model_id + "\t" + state;
    }
};

}  // namespace greenhouse

#endif  // GREENHOUSE_LOGGING_HPP
