#pragma once
#include "types.h"
#include "orderbook.h"
#include "csv_io.h"
#include "engine_sequential.h"   // Metrics, get_peak_rss_kb(), format_mbp_row()
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

namespace obr {

// ─── Double-buffered async writer ─────────────────────────────────
// Thread 1 (hot) formats rows into buf[active].
// When a buffer fills, it signals the writer thread and swaps.
// Writer thread writes the full buffer to disk while hot thread continues.
class AsyncDoubleWriter {
    static constexpr size_t BUF_SIZE = 4 * 1024 * 1024; // 4MB each
    
    char* bufs_[2];
    size_t pos_;
    int active_;   // 0 or 1
    int fd_;
    
    // Communication with writer thread
    std::thread writer_thread_;
    alignas(64) std::atomic<int> pending_buf_{-1};   // buffer index to write, -1 = none
    alignas(64) std::atomic<size_t> pending_len_{0};
    alignas(64) std::atomic<bool> done_{false};

public:
    AsyncDoubleWriter() : pos_(0), active_(0), fd_(-1) {
        bufs_[0] = static_cast<char*>(std::aligned_alloc(4096, BUF_SIZE));
        bufs_[1] = static_cast<char*>(std::aligned_alloc(4096, BUF_SIZE));
    }
    
    ~AsyncDoubleWriter() {
        close();
        std::free(bufs_[0]);
        std::free(bufs_[1]);
    }

    bool open(const char* path) {
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) return false;
        
        // Start writer thread
        writer_thread_ = std::thread([this]() {
            while (true) {
                int buf = pending_buf_.load(std::memory_order_acquire);
                if (buf >= 0) {
                    size_t len = pending_len_.load(std::memory_order_relaxed);
                    size_t written = 0;
                    while (written < len) {
                        ssize_t r = ::write(fd_, bufs_[buf] + written, len - written);
                        if (r < 0) break;
                        written += r;
                    }
                    pending_buf_.store(-1, std::memory_order_release);
                } else if (done_.load(std::memory_order_acquire)) {
                    break;
                }
                // Tight spin — writer thread is I/O bound anyway
            }
        });
        
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            // Flush remaining data in active buffer
            if (pos_ > 0) {
                // Wait for any pending write to finish
                while (pending_buf_.load(std::memory_order_acquire) >= 0) {}
                // Write directly
                size_t written = 0;
                while (written < pos_) {
                    ssize_t r = ::write(fd_, bufs_[active_] + written, pos_ - written);
                    if (r < 0) break;
                    written += r;
                }
                pos_ = 0;
            }
            done_.store(true, std::memory_order_release);
            if (writer_thread_.joinable()) writer_thread_.join();
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Get write pointer for direct formatting
    __attribute__((always_inline))
    char* ptr() { return bufs_[active_] + pos_; }

    __attribute__((always_inline))
    void advance(size_t n) { pos_ += n; }

    // Ensure space, swapping buffers if needed
    __attribute__((always_inline))
    void ensure(size_t n) {
        if (__builtin_expect(pos_ + n >= BUF_SIZE, 0)) {
            swap_and_flush();
        }
    }

    void write(const char* s, size_t len) {
        if (__builtin_expect(pos_ + len >= BUF_SIZE, 0)) swap_and_flush();
        std::memcpy(bufs_[active_] + pos_, s, len);
        pos_ += len;
    }

    void write(const char* s) { write(s, std::strlen(s)); }

    void write_mbp_header() {
        write(",ts_recv,ts_event,rtype,publisher_id,instrument_id,"
              "action,side,depth,price,size,flags,ts_in_delta,sequence");
        for (int i = 0; i < MAX_DEPTH; i++) {
            char b[80];
            std::snprintf(b, sizeof(b),
                ",bid_px_%02d,bid_sz_%02d,bid_ct_%02d,"
                "ask_px_%02d,ask_sz_%02d,ask_ct_%02d",
                i, i, i, i, i, i);
            write(b);
        }
        write(",symbol,order_id\n");
    }

private:
    void swap_and_flush() {
        // Wait for previous write to finish
        while (pending_buf_.load(std::memory_order_acquire) >= 0) {}
        
        // Submit current buffer for writing
        pending_len_.store(pos_, std::memory_order_relaxed);
        pending_buf_.store(active_, std::memory_order_release);
        
        // Swap to other buffer
        active_ ^= 1;
        pos_ = 0;
    }
};

// ─── Multithreaded Engine ─────────────────────────────────────────
// Architecture: 1 hot thread (parse + process + format) + 1 I/O thread.
//
// The hot thread does everything: parse the MBO row, apply to orderbook,
// and format the output directly into a double-buffered write region.
// The I/O thread asynchronously writes filled buffers to disk.
//
// This eliminates:
// - MbpSnapshot struct (was ~1.1KB per row)
// - Two SPSC queues and their atomic overhead
// - 960 bytes of memcpy per row for bid/ask snapshot data
// - One entire thread's worth of cache contention
//
// The only synchronization is a single atomic int when buffers swap
// (~every 4MB = every ~8000 rows), not per-row.
class MultithreadedEngine {
public:
    Metrics run(const char* input_path, const char* output_path) {
        using Clock = std::chrono::high_resolution_clock;
        Metrics m;
        auto t0 = Clock::now();

        MmapReader reader;
        if (!reader.open(input_path)) {
            std::fprintf(stderr, "Error: cannot open %s\n", input_path);
            return m;
        }
        reader.skip_line();

        AsyncDoubleWriter writer;
        if (!writer.open(output_path)) {
            std::fprintf(stderr, "Error: cannot open output %s\n", output_path);
            reader.close();
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

        m.total_time_us   = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
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
