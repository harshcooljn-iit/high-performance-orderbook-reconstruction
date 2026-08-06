#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>

namespace obr {

class Validator {
public:
    struct Result {
        bool passed            = false;
        int  total_lines       = 0;
        int  mismatches        = 0;
        int  first_mismatch_line = -1;
        std::string expected_line;
        std::string actual_line;
    };

    static Result validate(const char* ref_path, const char* gen_path) {
        Result r;
        std::ifstream ref(ref_path);
        std::ifstream gen(gen_path);

        if (!ref.is_open()) {
            std::fprintf(stderr, "Error: cannot open reference %s\n", ref_path);
            return r;
        }
        if (!gen.is_open()) {
            std::fprintf(stderr, "Error: cannot open generated %s\n", gen_path);
            return r;
        }

        std::string rl, gl;
        int line = 0;

        while (std::getline(ref, rl)) {
            line++;
            if (!std::getline(gen, gl)) {
                r.mismatches++;
                if (r.first_mismatch_line < 0) {
                    r.first_mismatch_line = line;
                    r.expected_line = rl;
                    r.actual_line   = "<EOF>";
                }
                continue;
            }

            // Strip trailing \r / \n
            while (!rl.empty() && (rl.back() == '\r' || rl.back() == '\n')) rl.pop_back();
            while (!gl.empty() && (gl.back() == '\r' || gl.back() == '\n')) gl.pop_back();

            if (rl != gl) {
                r.mismatches++;
                if (r.first_mismatch_line < 0) {
                    r.first_mismatch_line = line;
                    r.expected_line = rl;
                    r.actual_line   = gl;
                }
            }
        }

        while (std::getline(gen, gl)) {
            line++;
            r.mismatches++;
            if (r.first_mismatch_line < 0) {
                r.first_mismatch_line = line;
                r.expected_line = "<EOF>";
                r.actual_line   = gl;
            }
        }

        r.total_lines = line;
        r.passed = (r.mismatches == 0);
        return r;
    }

    static void print_result(const Result& r) {
        std::printf("\n=== Validation Result ===\n");
        std::printf("Total lines compared: %d\n", r.total_lines);
        if (r.passed) {
            std::printf("Status: PASSED ✓\n");
            std::printf("All lines match exactly.\n");
        } else {
            std::printf("Status: FAILED ✗\n");
            std::printf("Mismatches: %d\n", r.mismatches);
            std::printf("First mismatch at line %d:\n", r.first_mismatch_line);
            std::printf("  Expected: %.200s...\n", r.expected_line.c_str());
            std::printf("  Actual:   %.200s...\n", r.actual_line.c_str());
        }
        std::printf("========================\n\n");
    }
};

} // namespace obr
