#pragma once
#include "types.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace obr {

// ─── Memory-mapped file reader ────────────────────────────────────
class MmapReader {
public:
    MmapReader() = default;
    ~MmapReader() { close(); }

    bool open(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st;
        if (fstat(fd_, &st) < 0) { ::close(fd_); return false; }
        size_ = static_cast<size_t>(st.st_size);
        data_ = static_cast<const char*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) { ::close(fd_); return false; }
        madvise((void*)data_, size_, MADV_SEQUENTIAL);
        pos_ = data_;
        end_ = data_ + size_;
        return true;
    }

    void close() {
        if (data_ && data_ != MAP_FAILED) { munmap((void*)data_, size_); data_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool eof() const { return pos_ >= end_; }

    void skip_line() {
        const char* p = static_cast<const char*>(memchr(pos_, '\n', end_ - pos_));
        if (p) pos_ = p + 1;
        else pos_ = end_;
    }

    const char* pos() const { return pos_; }
    const char* end() const { return end_; }
    void set_pos(const char* p) { pos_ = p; }

private:
    int         fd_   = -1;
    const char* data_ = nullptr;
    const char* pos_  = nullptr;
    const char* end_  = nullptr;
    size_t      size_ = 0;
};

// ─── Fast integer parsing ─────────────────────────────────────────
__attribute__((always_inline))
inline int fast_atoi(const char* s, int len) {
    if (__builtin_expect(len == 0, 0)) return 0;
    int val = 0;
    for (int i = 0; i < len; i++) val = val * 10 + (s[i] - '0');
    return val;
}

__attribute__((always_inline))
inline uint64_t fast_atou64(const char* s, int len) {
    if (__builtin_expect(len == 0, 0)) return 0;
    uint64_t val = 0;
    for (int i = 0; i < len; i++) val = val * 10 + static_cast<uint64_t>(s[i] - '0');
    return val;
}



// ─── Ultra-fast single-pass CSV parser ────────────────────────────
// One scan to find all commas, then extract fields by pointer arithmetic.
// Fused price parsing (fixed-point + trim in single pass).
__attribute__((hot))
inline bool parse_mbo_row(MmapReader& reader, MboRecord& rec) {
    if (__builtin_expect(reader.eof(), 0)) return false;
    
    const char* start = reader.pos();
    const char* end = reader.end();
    
    // Find newline using memchr (hardware-accelerated on modern CPUs)
    const char* nl = static_cast<const char*>(memchr(start, '\n', end - start));
    if (__builtin_expect(!nl, 0)) nl = end;
    reader.set_pos(nl < end ? nl + 1 : end);
    if (__builtin_expect(start == nl, 0)) return false;

    rec.raw = start;

    // Single pass: find all commas at once
    const char* commas[14];
    int c_idx = 0;
    for (const char* ptr = start; ptr < nl; ptr++) {
        if (*ptr == ',') {
            commas[c_idx++] = ptr;
            if (c_idx == 14) break;
        }
    }
    if (__builtin_expect(c_idx < 13, 0)) return false;

    // Extract fields by pointer arithmetic — zero-copy
    rec.ts_event_idx = commas[0] - start + 1;
    rec.publisher_idx = commas[2] - start + 1;
    rec.action_idx = commas[4] - start + 1;

    rec.action = static_cast<Action>(*(commas[4] + 1));
    rec.side = static_cast<Side>(*(commas[5] + 1));

    // Fused price parse: fixed-point conversion + string trim in one pass
    const char* price_start = commas[6] + 1;
    int price_len = commas[7] - price_start;
    parse_price_fused(price_start, price_len, rec.price, rec.price_str, rec.price_len);

    rec.size_str = commas[7] + 1;
    rec.size_len = commas[8] - rec.size_str;
    rec.size = fast_atoi(rec.size_str, rec.size_len);

    rec.order_id_str = commas[9] + 1;
    rec.order_id_len = commas[10] - rec.order_id_str;
    rec.order_id = fast_atou64(rec.order_id_str, rec.order_id_len);

    rec.flags_str = commas[10] + 1;
    rec.flags_len = commas[13] - rec.flags_str;

    rec.symbol_str = commas[13] + 1;
    uint16_t sym_len = nl - rec.symbol_str;
    if (sym_len > 0 && rec.symbol_str[sym_len - 1] == '\r') sym_len--;
    rec.symbol_len = sym_len;

    return true;
}

// ─── Buffered writer ──────────────────────────────────────────────
class CsvWriter {
public:
    static constexpr size_t BUF_SIZE = 4 * 1024 * 1024; // 4MB

    CsvWriter() : fd_(-1), pos_(0) {
        buf_ = static_cast<char*>(std::aligned_alloc(4096, BUF_SIZE));
    }
    ~CsvWriter() { close(); std::free(buf_); }

    bool open(const char* path) {
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        return fd_ >= 0;
    }

    void close() {
        if (fd_ >= 0) { flush(); ::close(fd_); fd_ = -1; }
    }

    void flush() {
        if (pos_ > 0) {
            size_t written = 0;
            while (written < pos_) {
                ssize_t r = ::write(fd_, buf_ + written, pos_ - written);
                if (r < 0) break;
                written += r;
            }
            pos_ = 0;
        }
    }

    __attribute__((always_inline))
    char* ptr() { return buf_ + pos_; }

    __attribute__((always_inline))
    void advance(size_t n) { pos_ += n; }

    __attribute__((always_inline))
    void ensure(size_t n) {
        if (__builtin_expect(pos_ + n >= BUF_SIZE, 0)) flush();
    }

    inline void put(char c) {
        if (__builtin_expect(pos_ >= BUF_SIZE, 0)) flush();
        buf_[pos_++] = c;
    }

    inline void write(const char* s, size_t len) {
        if (__builtin_expect(pos_ + len >= BUF_SIZE, 0)) flush();
        std::memcpy(buf_ + pos_, s, len);
        pos_ += len;
    }

    inline void write(const char* s) { write(s, std::strlen(s)); }

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
    char*  buf_;
    int    fd_;
    size_t pos_;
};

} // namespace obr
