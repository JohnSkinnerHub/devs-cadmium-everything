/**
 * @file check.hpp
 * @brief A minimal self-checking helper so every demo can verify what it claims.
 *
 * Each demo is both a *tutorial* (it prints what it is doing) and a *test* (it CHECKs the
 * result and exits non-zero if anything is wrong). `make test` runs them all.
 */
#ifndef GREENHOUSE_CHECK_HPP
#define GREENHOUSE_CHECK_HPP

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

namespace greenhouse {

inline int& check_failures() {
    static int failures = 0;
    return failures;
}
inline int& check_count() {
    static int count = 0;
    return count;
}

inline void check_impl(bool ok, const std::string& what, const char* file, int line) {
    ++check_count();
    if (!ok) {
        ++check_failures();
        std::cout << "  [FAIL] " << what << "   (" << file << ":" << line << ")" << std::endl;
    } else {
        std::cout << "  [ ok ] " << what << std::endl;
    }
}

template <typename A, typename B>
void check_eq_impl(const A& a, const B& b, const std::string& what, const char* file, int line) {
    std::ostringstream oss;
    bool ok = (a == b);
    oss << what;
    if (!ok) oss << "   expected <" << b << "> got <" << a << ">";
    check_impl(ok, oss.str(), file, line);
}

inline void check_near_impl(double a, double b, double tol, const std::string& what, const char* file, int line) {
    std::ostringstream oss;
    bool ok = std::fabs(a - b) <= tol;
    oss << what;
    if (!ok) oss << "   expected <" << b << "> got <" << a << ">";
    check_impl(ok, oss.str(), file, line);
}

/// Print a section header.
inline void section(const std::string& title) {
    std::cout << "\n=== " << title << " ===" << std::endl;
}

/// Final line of every demo: summary and process exit code.
inline int check_summary(const char* program) {
    std::cout << "\n" << program << ": " << (check_count() - check_failures()) << "/" << check_count()
              << " checks passed" << std::endl;
    return check_failures() == 0 ? 0 : 1;
}

}  // namespace greenhouse

// Variadic so that commas inside template argument lists (`f<A, B>(x)`) do not split the argument.
#define CHECK(...) ::greenhouse::check_impl((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) ::greenhouse::check_impl((cond), (msg), __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::greenhouse::check_eq_impl((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) ::greenhouse::check_near_impl((a), (b), (tol), #a " ~= " #b, __FILE__, __LINE__)
#define CHECK_THROWS(expr, exception_type)                                                     \
    do {                                                                                       \
        bool threw_ = false;                                                                   \
        try {                                                                                  \
            expr;                                                                              \
        } catch (const exception_type&) {                                                      \
            threw_ = true;                                                                     \
        }                                                                                      \
        ::greenhouse::check_impl(threw_, "throws " #exception_type ": " #expr, __FILE__, __LINE__); \
    } while (0)

#endif  // GREENHOUSE_CHECK_HPP
