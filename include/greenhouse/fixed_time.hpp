/**
 * @file fixed_time.hpp
 * @brief A user-defined simulation-time type (integer milliseconds with +infinity).
 *
 * WHAT
 *   `greenhouse::fixed_time` is a drop-in replacement for `float` / `double` as the TIME
 *   template argument of every model, engine and logger in this project.
 *
 * WHY
 *   One of Cadmium's headline features is that "time representation is independent of
 *   model implementation": a model is written as `template<typename TIME> class my_model`
 *   and never says what TIME is. The *runner* picks it. This file proves the point by
 *   providing a third representation besides `float` and `double`, and the demos run the
 *   very same models with all three and check that the event traces agree.
 *
 *   Floating-point time has a well-known drawback in discrete-event simulation: `0.1 + 0.2
 *   != 0.3`, so two events that *should* be simultaneous (and therefore trigger a
 *   confluent transition) may be off by one ULP and be processed as two separate events.
 *   Integer ticks have no such problem, so this type is also the "production grade"
 *   choice for this project.
 *
 * WHAT CADMIUM EXPECTS OF A TIME TYPE (derived from reading the headers)
 *   - default-constructible, and the default value is time zero          (`TIME{}`, `TIME()`)
 *   - implicitly constructible from a floating literal                   (`return 1.0;`)
 *   - constructible from an int via braces                               (`TIME({0})`)
 *   - `+  -  +=  -=`  and the six comparison operators                   (engines, cells)
 *   - `operator<<` (logging) and `operator>>` (stock `iestream_input` file reader)
 *   - `std::numeric_limits<TIME>::infinity()` + `has_infinity` for passive states and for
 *     `run_until_passivate()`
 *   - `std::hash<TIME>` and `==` because the Cell-DEVS *transport* delay buffer keeps an
 *     `std::unordered_map<TIME, State>`
 *
 *   Note: Cadmium's compile-time concept checks always instantiate a model with
 *   `float` (see `atomic_model_assert`), so every model must ALSO compile with float.
 */
#ifndef GREENHOUSE_FIXED_TIME_HPP
#define GREENHOUSE_FIXED_TIME_HPP

#include <cmath>
#include <cstdint>
#include <functional>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace greenhouse {

/**
 * Simulation time as a signed 64-bit count of milliseconds.
 * +infinity and -infinity are represented by the extreme values and are *sticky*
 * (infinity plus or minus any finite value stays infinity), which is exactly the
 * behaviour passive DEVS states (`ta = infinity`) rely on.
 */
class fixed_time {
public:
    using rep = std::int64_t;
    static constexpr rep ticks_per_second = 1000;

private:
    static constexpr rep kPosInf = std::numeric_limits<rep>::max();
    static constexpr rep kNegInf = std::numeric_limits<rep>::min();
    rep ticks_;

    struct raw_tag {};
    constexpr fixed_time(raw_tag, rep ticks) noexcept : ticks_(ticks) {}

    constexpr bool is_pos_inf() const noexcept { return ticks_ == kPosInf; }
    constexpr bool is_neg_inf() const noexcept { return ticks_ == kNegInf; }

public:
    /// Time zero (the "default value" Cadmium models use for `TIME{}`).
    constexpr fixed_time() noexcept : ticks_(0) {}

    /// Implicit conversion from seconds, so models can simply `return 1.0;`.
    fixed_time(double seconds) {  // NOLINT: implicit on purpose
        if (std::isnan(seconds)) throw std::domain_error("fixed_time: NaN is not a time");
        if (std::isinf(seconds)) ticks_ = seconds > 0 ? kPosInf : kNegInf;
        else ticks_ = static_cast<rep>(std::llround(seconds * ticks_per_second));
    }

    static constexpr fixed_time infinity() noexcept { return fixed_time(raw_tag{}, kPosInf); }
    static constexpr fixed_time from_ticks(rep t) noexcept { return fixed_time(raw_tag{}, t); }

    constexpr rep ticks() const noexcept { return ticks_; }
    constexpr bool is_infinite() const noexcept { return is_pos_inf() || is_neg_inf(); }
    double seconds() const noexcept {
        if (is_pos_inf()) return std::numeric_limits<double>::infinity();
        if (is_neg_inf()) return -std::numeric_limits<double>::infinity();
        return static_cast<double>(ticks_) / ticks_per_second;
    }

    // ---- arithmetic (saturating at +-infinity; inf - inf is undefined, like NaN) -------
    friend fixed_time operator+(fixed_time a, fixed_time b) {
        if (a.is_infinite() || b.is_infinite()) {
            if (a.is_infinite() && b.is_infinite() && a.ticks_ != b.ticks_)
                throw std::domain_error("fixed_time: inf + -inf is undefined");
            return a.is_infinite() ? a : b;
        }
        return fixed_time(raw_tag{}, a.ticks_ + b.ticks_);
    }
    friend fixed_time operator-(fixed_time a, fixed_time b) {
        if (a.is_infinite() || b.is_infinite()) {
            if (a.is_infinite() && b.is_infinite() && a.ticks_ == b.ticks_)
                throw std::domain_error("fixed_time: inf - inf is undefined");
            if (a.is_infinite()) return a;
            return b.is_pos_inf() ? fixed_time(raw_tag{}, kNegInf) : fixed_time(raw_tag{}, kPosInf);
        }
        return fixed_time(raw_tag{}, a.ticks_ - b.ticks_);
    }
    fixed_time& operator+=(fixed_time o) { return *this = *this + o; }
    fixed_time& operator-=(fixed_time o) { return *this = *this - o; }

    // ---- comparison ------------------------------------------------------------------
    friend constexpr bool operator==(fixed_time a, fixed_time b) noexcept { return a.ticks_ == b.ticks_; }
    friend constexpr bool operator!=(fixed_time a, fixed_time b) noexcept { return a.ticks_ != b.ticks_; }
    friend constexpr bool operator<(fixed_time a, fixed_time b) noexcept { return a.ticks_ < b.ticks_; }
    friend constexpr bool operator>(fixed_time a, fixed_time b) noexcept { return a.ticks_ > b.ticks_; }
    friend constexpr bool operator<=(fixed_time a, fixed_time b) noexcept { return a.ticks_ <= b.ticks_; }
    friend constexpr bool operator>=(fixed_time a, fixed_time b) noexcept { return a.ticks_ >= b.ticks_; }

    // ---- streaming: logs print seconds (e.g. "2.5"), files are read as seconds ---------
    friend std::ostream& operator<<(std::ostream& os, fixed_time t) {
        if (t.is_pos_inf()) return os << "inf";
        if (t.is_neg_inf()) return os << "-inf";
        return os << t.seconds();
    }
    friend std::istream& operator>>(std::istream& is, fixed_time& t) {
        double s;
        if (is >> s) t = fixed_time(s);
        return is;
    }
};

}  // namespace greenhouse

namespace std {

/// Lets `std::numeric_limits<greenhouse::fixed_time>::infinity()` work (stock passive models,
/// `run_until_passivate`, Cell-DEVS delay buffers all use it).
template <>
class numeric_limits<greenhouse::fixed_time> {
public:
    static constexpr bool is_specialized = true;
    static constexpr bool has_infinity = true;
    static constexpr bool is_signed = true;
    static constexpr greenhouse::fixed_time infinity() noexcept { return greenhouse::fixed_time::infinity(); }
    static constexpr greenhouse::fixed_time max() noexcept {
        return greenhouse::fixed_time::from_ticks(numeric_limits<greenhouse::fixed_time::rep>::max() - 1);
    }
    static constexpr greenhouse::fixed_time lowest() noexcept {
        return greenhouse::fixed_time::from_ticks(numeric_limits<greenhouse::fixed_time::rep>::min() + 1);
    }
};

/// Required by the Cell-DEVS transport delay buffer (`std::unordered_map<TIME, state>`).
template <>
struct hash<greenhouse::fixed_time> {
    size_t operator()(greenhouse::fixed_time t) const noexcept { return hash<long long>()(t.ticks()); }
};

}  // namespace std

#endif  // GREENHOUSE_FIXED_TIME_HPP
