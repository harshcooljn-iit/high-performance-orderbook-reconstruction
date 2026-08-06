#pragma once
#include "types.h"
#include "orderbook.h"
#include "csv_io.h"
#include "engine_sequential.h"   // Metrics, get_peak_rss_kb()
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

namespace obr {

// ═══════════════════════════════════════════════════════════════════════
//  Lock-free SPSC ring buffer
// ═══════════════════════════════════════════════════════════════════════
template <typename T, size_t Capacity>
class SPSCRingBuffer {
public:
    SPSCRingBuffer() : head_(0), tail_(0) {}

    bool try_push(const T& item) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t next = (h + 1) % Capacity;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        item = buf_[t];
        tail_.store((t + 1) % Capacity, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    T buf_[Capacity];
};

// ═══════════════════════════════════════════════════════════════════════
//  NOTE – Why we do NOT parallelise array shifts
//  The top-12 array has at most 12 PriceLevel elements.
//  memmove/loop-shift for this is 1-2 cache-line copies.
//  Thread synchronisation overhead far exceeds the shift cost.
// ═══════════════════════════════════════════════════════════════════════

class MultithreadedEngine {
public:
    static constexpr size_t RING = 8192;

    Metrics run(const char* input_path, const char* output_path) {
        using Clock = std::chrono::high_resolution_clock;
        Metrics m;
        auto t0 = Clock::now();

        SPSCRingBuffer<MboRecord, RING>  read_buf;
        SPSCRingBuffer<MbpRecord, RING>  write_buf;
        std::atomic<bool> read_done{false};
        std::atomic<bool> proc_done{false};
        std::atomic<size_t> rows_read{0};
        std::atomic<size_t> rows_written{0};
        double rd_us = 0, pr_us = 0, wr_us = 0;

        // ── Reader ────────────────────────────────────────────────
        std::thread t_reader([&]() {
            auto s = Clock::now();
            MmapReader reader;
            if (!reader.open(input_path)) {
                std::fprintf(stderr, "Error: cannot open %s\n", input_path);
                read_done.store(true, std::memory_order_release);
                return;
            }
            reader.skip_line();
            MboRecord rec;
            while (parse_mbo_row(reader, rec)) {
                rows_read.fetch_add(1, std::memory_order_relaxed);
                while (!read_buf.try_push(rec)) {}
            }
            reader.close();
            read_done.store(true, std::memory_order_release);
            rd_us = std::chrono::duration<double, std::micro>(Clock::now() - s).count();
        });

        // ── Processor ─────────────────────────────────────────────
        std::thread t_proc([&]() {
            auto s = Clock::now();
            Orderbook book;
            int row_idx = 0;
            MboRecord mbo;

            while (true) {
                if (!read_buf.try_pop(mbo)) {
                    if (read_done.load(std::memory_order_acquire) && read_buf.empty())
                        break;
                    continue;
                }

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

                    while (!write_buf.try_push(mbp)) {}
                }
            }

            proc_done.store(true, std::memory_order_release);
            pr_us = std::chrono::duration<double, std::micro>(Clock::now() - s).count();
        });

        // ── Writer ────────────────────────────────────────────────
        std::thread t_writer([&]() {
            auto s = Clock::now();
            CsvWriter writer;
            if (!writer.open(output_path)) {
                std::fprintf(stderr, "Error: cannot open output %s\n", output_path);
                return;
            }
            writer.write_mbp_header();
            MbpRecord mbp;
            while (true) {
                if (!write_buf.try_pop(mbp)) {
                    if (proc_done.load(std::memory_order_acquire) && write_buf.empty())
                        break;
                    continue;
                }
                writer.write_mbp_row(mbp);
                rows_written.fetch_add(1, std::memory_order_relaxed);
            }
            writer.close();
            wr_us = std::chrono::duration<double, std::micro>(Clock::now() - s).count();
        });

        t_reader.join();
        t_proc.join();
        t_writer.join();

        m.total_time_us   = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        m.read_time_us    = rd_us;
        m.process_time_us = pr_us;
        m.write_time_us   = wr_us;
        m.total_rows_in   = rows_read.load();
        m.total_rows_out  = rows_written.load();
        m.avg_per_row_us  = m.total_time_us / std::max(m.total_rows_in, size_t(1));
        m.peak_memory_kb  = get_peak_rss_kb();
        return m;
    }
};

} // namespace obr
