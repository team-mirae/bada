// The tests' few helpers: a check that counts, and a summary line.
#pragma once

#include <cstdio>
#include <string>

namespace test {

inline int checks = 0;
inline int failures = 0;

inline void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

inline void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    Check(got == want, what + ": got \"" + got + "\", want \"" + want + "\"");
}

inline int Finish(const char* name) {
    std::printf("%s: %d checks, %d failed\n", name, checks, failures);
    return failures ? 1 : 0;
}

}  // namespace test
