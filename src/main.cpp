#include "engine_sequential.h"
#include "engine_multithreaded.h"
#include "validator.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace obr;

static void print_usage(const char* prog) {
    std::printf("Usage: %s <mbo_input.csv> [--reference <mbp_reference.csv>]\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char* input_path = argv[1];
    const char* reference_path = nullptr;

    // std::printf("Starting program with input: %s\n", input_path);
    // fflush(stdout);

    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc)
            reference_path = argv[++i];
    }

    // Derive output directory
    std::string inp(input_path);
    std::string dir = ".";
    auto sl = inp.rfind('/');
    if (sl != std::string::npos) dir = inp.substr(0, sl);

    std::string seq_out = dir + "/mbp_sequential.csv";
    std::string mt_out = dir + "/mbp_multithreaded.csv";

    std::printf(
        "\n╔══════════════════════════════════════════════════════════════╗\n"
        "║       MBO → MBP-10 Orderbook Reconstruction Engine         ║\n"
        "╚══════════════════════════════════════════════════════════════╝\n\n");
    std::printf("Input:  %s\n", input_path);
    std::printf("Output: %s  (sequential)\n", seq_out.c_str());
    std::printf("        %s  (multithreaded)\n\n", mt_out.c_str());

    // ── Sequential ────────────────────────────────────────────────
    std::printf("Running Sequential Engine...\n");
    SequentialEngine seq;
    Metrics sm = seq.run(input_path, seq_out.c_str());
    sm.print("Sequential");

    // ── Multithreaded ─────────────────────────────────────────────
    std::printf("Running Multithreaded Engine...\n");
    MultithreadedEngine mt;
    Metrics mm = mt.run(input_path, mt_out.c_str());
    mm.print("Multithreaded");

    // ── Comparison ────────────────────────────────────────────────
    std::printf(
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║                    Speed Comparison                        ║\n"
        "╠══════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Sequential:    %10.2f us  (%8.4f ms)               ║\n",
        sm.total_time_us, sm.total_time_us / 1000.0);
    std::printf("║  Multithreaded: %10.2f us  (%8.4f ms)               ║\n",
        mm.total_time_us, mm.total_time_us / 1000.0);
    double speedup = sm.total_time_us / mm.total_time_us;
    std::printf("║  Speedup:       %.2fx  %s                            ║\n",
        speedup, speedup > 1.0 ? "(MT faster)" : "(Seq faster)");
    std::printf(
        "╚══════════════════════════════════════════════════════════════╝\n");

    // ── Validation ────────────────────────────────────────────────
    if (reference_path) {
        std::printf("\nValidating Sequential output against reference...\n");
        auto sr = Validator::validate(reference_path, seq_out.c_str());
        Validator::print_result(sr);

        std::printf("Validating Multithreaded output against reference...\n");
        auto mr = Validator::validate(reference_path, mt_out.c_str());
        Validator::print_result(mr);

        if (!sr.passed || !mr.passed) return 1;
    }
    else {
        std::printf("\n[Skipping validation – no reference file provided]\n");
        std::printf("Use --reference data/mbp.csv to validate output\n");
    }

    return 0;
}
