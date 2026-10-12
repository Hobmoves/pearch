#pragma once

// Minimal test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::cerr << "    " << file << ":" << line << ": " << msg << "\n";
}

}  // namespace check

#define CHECK_CONCAT2(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT2(a, b)
#define TEST(name)                                                                  \
    static void CHECK_CONCAT(test_, name)();                                        \
    static check::Register CHECK_CONCAT(reg_, name)(#name, CHECK_CONCAT(test_, name)); \
    static void CHECK_CONCAT(test_, name)()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) check::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto va_ = (a);                                                                \
        const auto vb_ = (b);                                                                \
        if (!(va_ == vb_)) {                                                               \
            std::ostringstream os_;                                                        \
            os_ << "CHECK_EQ(" #a ", " #b "): " << va_ << " != " << vb_;                   \
            check::fail(__FILE__, __LINE__, os_.str());                                    \
        }                                                                                  \
    } while (0)
