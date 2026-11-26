// Minimal dependency-free test harness (keeps the vcpkg manifest small).
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace th {

struct Test {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

}  // namespace th

#define TH_CAT2(a, b) a##b
#define TH_CAT(a, b) TH_CAT2(a, b)
#define TEST_CASE(name)                                                       \
    static void TH_CAT(test_fn_, __LINE__)();                                 \
    static th::Registrar TH_CAT(test_reg_, __LINE__)(name, TH_CAT(test_fn_, __LINE__)); \
    static void TH_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++th::failures();                                                              \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #cond "\n";    \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto& va_ = (a);                                                             \
        const auto& vb_ = (b);                                                             \
        if (!(va_ == vb_)) {                                                               \
            ++th::failures();                                                              \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #a " == " #b   \
                      << "  (" << va_ << " vs " << vb_ << ")\n";                           \
        }                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                              \
    do {                                                                                   \
        const double va_ = double(a), vb_ = double(b);                                     \
        if (!(std::abs(va_ - vb_) <= (eps))) {                                             \
            ++th::failures();                                                              \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #a " ~= " #b   \
                      << "  (" << va_ << " vs " << vb_ << ")\n";                           \
        }                                                                                  \
    } while (0)
