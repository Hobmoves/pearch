#include <chrono>
#include <cstring>

#include "check.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed_cases = 0;
    for (const auto& c : check::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = check::failures();
        auto t0 = std::chrono::steady_clock::now();
        c.fn();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++run;
        bool ok = check::failures() == before;
        if (!ok) ++failed_cases;
        std::cout << (ok ? "  ok    " : "  FAIL  ") << c.name << "  (" << ms << " ms)\n";
    }
    std::cout << "\n" << run - failed_cases << "/" << run << " tests passed\n";
    return failed_cases == 0 ? 0 : 1;
}
