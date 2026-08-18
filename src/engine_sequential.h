#pragma once
#include "types.h"
#include "orderbook.h"
#include "csv_io.h"
#include <chrono>
#include <cstdio>
#include <sys/resource.h>
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
        std::printf("Total time:          %.2f us  (%.4f ms)\n", total_time_us, total_time_us / 1000.0);
        std::printf("  Read/Proc time:    %.2f us  (%.4f ms)\n", process_time_us, process_time_us / 1000.0);
        std::printf("  Write time:        %.2f us  (%.4f ms)\n", write_time_us, write_time_us / 1000.0);
        std::printf("Avg per row:         %.4f us  (%d ns)\n", avg_per_row_us, static_cast<int>(avg_per_row_us * 1000));
        std::printf("Peak memory:         %zu KB\n", peak_memory_kb);
        std::printf("========================\n\n");
    }
};

inline size_t get_peak_rss_kb() {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    // On macOS, ru_maxrss is in bytes; on Linux, it's in KB
#ifdef __APPLE__
    return static_cast<size_t>(u.ru_maxrss) / 1024;
#else
    return static_cast<size_t>(u.ru_maxrss);
#endif
}

// ─── Inline: format one MBP row directly into buffer ──────────────
// This is the HOT function. Every cycle counts.
__attribute__((hot, always_inline))
inline char* format_mbp_row(char* __restrict__ p, int row_index,
                             const MboRecord& mbo, const Orderbook& book) {
    // row_index
    {
        unsigned uval = static_cast<unsigned>(row_index);
        if (uval == 0) { *p++ = '0'; }
        else {
            char tmp[12]; int len = 0;
            while (uval > 0) { tmp[len++] = '0' + (uval % 10); uval /= 10; }
            for (int i = len - 1; i >= 0; i--) *p++ = tmp[i];
        }
    }
    *p++ = ',';

    // ts_recv = ts_event, ts_event = ts_event
    const char* ts_evt = mbo.raw + mbo.ts_event_idx;
    const char* ts_end = static_cast<const char*>(memchr(ts_evt, ',', 48));
    uint16_t ts_len = ts_end ? static_cast<uint16_t>(ts_end - ts_evt) : 0;
    
    std::memcpy(p, ts_evt, ts_len); p += ts_len;
    *p++ = ',';
    std::memcpy(p, ts_evt, ts_len); p += ts_len;

    // ,rtype=10,publisher_id,instrument_id,
    std::memcpy(p, ",10,", 4); p += 4;
    const char* pub = mbo.raw + mbo.publisher_idx;
    uint16_t pub_len = (mbo.raw + mbo.action_idx) - pub;
    std::memcpy(p, pub, pub_len); p += pub_len;

    // action, side
    *p++ = book.trade_pending() ? 'T' : static_cast<char>(mbo.action);
    *p++ = ',';
    *p++ = static_cast<char>(mbo.side);
    *p++ = ',';

    // depth
    {
        int d = book.depth();
        if (d == 0) { *p++ = '0'; }
        else {
            char tmp[4]; int len = 0;
            while (d > 0) { tmp[len++] = '0' + (d % 10); d /= 10; }
            for (int i = len - 1; i >= 0; i--) *p++ = tmp[i];
        }
    }
    *p++ = ',';

    // price
    if (mbo.price_len > 0) {
        std::memcpy(p, mbo.price_str, mbo.price_len);
        p += mbo.price_len;
    }
    *p++ = ',';

    // size
    std::memcpy(p, mbo.size_str, mbo.size_len);
    p += mbo.size_len;
    *p++ = ',';

    // flags,ts_in_delta,sequence
    std::memcpy(p, mbo.flags_str, mbo.flags_len);
    p += mbo.flags_len;

    // bid/ask levels — directly from orderbook state
    p = const_cast<Orderbook&>(book).write_levels(p);

    // ,symbol
    *p++ = ',';
    std::memcpy(p, mbo.symbol_str, mbo.symbol_len);
    p += mbo.symbol_len;

    // ,order_id
    *p++ = ',';
    if (book.trade_pending()) {
        *p++ = '0';
    } else {
        std::memcpy(p, mbo.order_id_str, mbo.order_id_len);
        p += mbo.order_id_len;
    }

    *p++ = '\n';
    return p;
}

// ─── Sequential Engine ────────────────────────────────────────────
// Single-threaded: parse → process → format directly into write buffer.
// No MbpSnapshot. No intermediate copies.
class SequentialEngine {
public:
    Metrics run(const char* input_path, const char* output_path) {
        using Clock = std::chrono::high_resolution_clock;
        Metrics m;
        auto t_total = Clock::now();

        MmapReader reader;
        if (!reader.open(input_path)) {
            std::fprintf(stderr, "Error: cannot open %s\n", input_path);
            return m;
        }
        reader.skip_line();

        CsvWriter writer;
        if (!writer.open(output_path)) {
            std::fprintf(stderr, "Error: cannot open output %s\n", output_path);
            return m;
        }
        writer.write_mbp_header();

        Orderbook book;
        int row_idx = 0;
        size_t total_in = 0;
        size_t total_out = 0;

        MboRecord mbo;
        auto t_proc_start = Clock::now();

        while (parse_mbo_row(reader, mbo)) {
            total_in++;
            bool should_output = book.apply(mbo);

            if (should_output) {
                // Ensure we have space for the largest possible row (~2KB)
                writer.ensure(2048);
                char* start = writer.ptr();
                char* end = format_mbp_row(start, row_idx++, mbo, book);
                writer.advance(end - start);
                book.reset_trade();
                total_out++;
            }
        }

        auto t_proc_end = Clock::now();
        double proc_us = std::chrono::duration<double, std::micro>(t_proc_end - t_proc_start).count();

        auto t_write = Clock::now();
        writer.close();
        reader.close();
        double write_flush_us = std::chrono::duration<double, std::micro>(Clock::now() - t_write).count();

        m.total_time_us   = std::chrono::duration<double, std::micro>(Clock::now() - t_total).count();
        m.read_time_us    = proc_us;
        m.process_time_us = proc_us;
        m.write_time_us   = write_flush_us;
        m.total_rows_in   = total_in;
        m.total_rows_out  = total_out;
        m.avg_per_row_us  = total_in > 0 ? (m.total_time_us / total_in) : 0;
        m.peak_memory_kb  = get_peak_rss_kb();

        return m;
    }
};

} // namespace obr
