#pragma once
#include "types.h"
#include "orderbook.h"
#include "csv_io.h"
#include <chrono>
#include <cstdio>
#include <sys/resource.h>
#include <vector>
#include <cstring>

namespace obr {

struct Metrics {
    double total_time_us   = 0;
    double read_time_us    = 0;
    double process_time_us = 0;
    double write_time_us   = 0;
    double avg_per_row_us  = 0;
    size_t total_rows_in   = 0;
    size_t total_rows_out  = 0;
    size_t peak_memory_kb  = 0;

    void print(const char* name) const {
        std::printf("\n=== %s Engine Metrics ===\n", name);
        std::printf("Total rows read:     %zu\n",  total_rows_in);
        std::printf("Total rows written:  %zu\n",  total_rows_out);
        std::printf("Total time:          %.2f us  (%.4f ms)\n",
                    total_time_us, total_time_us / 1000.0);
        std::printf("  Read time:         %.2f us  (%.4f ms)\n",
                    read_time_us,  read_time_us  / 1000.0);
        std::printf("  Process time:      %.2f us  (%.4f ms)\n",
                    process_time_us, process_time_us / 1000.0);
        std::printf("  Write time:        %.2f us  (%.4f ms)\n",
                    write_time_us, write_time_us / 1000.0);
        std::printf("Avg per row:         %.4f us\n", avg_per_row_us);
        std::printf("Peak memory:         %zu KB\n", peak_memory_kb);
        std::printf("========================\n\n");
    }
};

inline size_t get_peak_rss_kb() {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return static_cast<size_t>(u.ru_maxrss) / 1024;  // macOS: bytes
}

class SequentialEngine {
public:
    Metrics run(const char* input_path, const char* output_path) {
        using Clock = std::chrono::high_resolution_clock;
        Metrics m;
        auto t_total = Clock::now();

        // ── Read ──────────────────────────────────────────────────
        auto t_read = Clock::now();
        MmapReader reader;
        if (!reader.open(input_path)) {
            std::fprintf(stderr, "Error: cannot open %s\n", input_path);
            return m;
        }
        reader.skip_line();
        std::vector<MboRecord> records;
        records.reserve(8192);
        MboRecord rec;
        while (parse_mbo_row(reader, rec)) records.push_back(rec);
        reader.close();
        m.read_time_us  = us(Clock::now(), t_read);
        m.total_rows_in = records.size();

        // ── Process + Write ───────────────────────────────────────
        auto t_proc = Clock::now();
        CsvWriter writer;
        if (!writer.open(output_path)) {
            std::fprintf(stderr, "Error: cannot open output %s\n", output_path);
            return m;
        }
        writer.write_mbp_header();

        Orderbook book;
        int row_idx = 0;
        double w_acc = 0;

        for (size_t i = 0; i < records.size(); i++) {
            MboRecord& mbo = records[i];

            bool should_output = book.apply(mbo);

            if (should_output) {
                MbpRecord mbp;
                std::memset(&mbp, 0, sizeof(mbp));

                std::memcpy(mbp.ts_event, mbo.ts_event, sizeof(mbp.ts_event));
                mbp.rtype         = 10;
                mbp.publisher_id  = mbo.publisher_id;
                mbp.instrument_id = mbo.instrument_id;
                mbp.action        = mbo.action;
                mbp.side          = mbo.side;
                mbp.depth         = book.depth();
                std::memcpy(mbp.price_str, mbo.price_str, sizeof(mbp.price_str));
                mbp.size          = mbo.size;
                mbp.flags         = mbo.flags;
                mbp.ts_in_delta   = mbo.ts_in_delta;
                mbp.sequence      = mbo.sequence;
                std::memcpy(mbp.symbol, mbo.symbol, sizeof(mbp.symbol));
                mbp.order_id      = mbo.order_id;
                mbp.trade_pending = book.trade_pending();
                mbp.row_index     = row_idx++;

                book.snapshot(mbp.bids, mbp.asks);
                book.reset_trade();

                auto ws = Clock::now();
                writer.write_mbp_row(mbp);
                w_acc += us(Clock::now(), ws);
                m.total_rows_out++;
            }
        }

        m.process_time_us = us(Clock::now(), t_proc) - w_acc;
        m.write_time_us   = w_acc;
        writer.close();

        m.total_time_us  = us(Clock::now(), t_total);
        m.avg_per_row_us = m.total_rows_out > 0
            ? m.total_time_us / static_cast<double>(m.total_rows_out)
            : 0;
        m.peak_memory_kb = get_peak_rss_kb();
        return m;
    }

private:
    static double us(std::chrono::high_resolution_clock::time_point end,
                     std::chrono::high_resolution_clock::time_point start) {
        return std::chrono::duration<double, std::micro>(end - start).count();
    }
};

} // namespace obr
