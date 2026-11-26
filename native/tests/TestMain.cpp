#include "TestHarness.h"

int main() {
    int run = 0;
    for (const auto& t : th::registry()) {
        const int before = th::failures();
        std::cout << "[ RUN  ] " << t.name << "\n";
        t.fn();
        std::cout << (th::failures() == before ? "[  OK  ] " : "[ FAIL ] ") << t.name << "\n";
        ++run;
    }
    std::cout << "\n" << run << " tests, " << th::failures() << " failed checks\n";
    return th::failures() == 0 ? 0 : 1;
}
