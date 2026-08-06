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

// ═══════════════════════════════════════════════════════════════════════
//  Memory-mapped file reader
// ═══════════════════════════════════════════════════════════════════════
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
        data_ = static_cast<const char*>(
            mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) { ::close(fd_); return false; }
        madvise(const_cast<char*>(data_), size_, MADV_SEQUENTIAL);
        pos_ = data_;
        end_ = data_ + size_;
        return true;
    }

    void close() {
        if (data_ && data_ != MAP_FAILED) { munmap(const_cast<char*>(data_), size_); data_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool eof() const { return pos_ >= end_; }

    void skip_line() {
        while (pos_ < end_ && *pos_ != '\n') pos_++;
        if (pos_ < end_) pos_++;
    }

    const char* next_field(int& len) {
        const char* start = pos_;
        while (pos_ < end_ && *pos_ != ',' && *pos_ != '\n' && *pos_ != '\r')
            pos_++;
        len = static_cast<int>(pos_ - start);
        if (pos_ < end_ && *pos_ == ',') pos_++;
        return start;
    }

    void finish_line() {
        while (pos_ < end_ && *pos_ != '\n') pos_++;
        if (pos_ < end_) pos_++;
    }

private:
    int         fd_   = -1;
    const char* data_ = nullptr;
    const char* pos_  = nullptr;
    const char* end_  = nullptr;
    size_t      size_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════
//  Fast parsing
// ═══════════════════════════════════════════════════════════════════════
inline int fast_atoi(const char* s, int len) {
    if (len == 0) return 0;
    int val = 0, i = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; i++; }
    for (; i < len; i++) val = val * 10 + (s[i] - '0');
    return neg ? -val : val;
}

inline uint64_t fast_atou64(const char* s, int len) {
    if (len == 0) return 0;
    uint64_t val = 0;
    for (int i = 0; i < len; i++) val = val * 10 + static_cast<uint64_t>(s[i] - '0');
    return val;
}

// ═══════════════════════════════════════════════════════════════════════
//  Parse a single MBO CSV row
// ═══════════════════════════════════════════════════════════════════════
inline bool parse_mbo_row(MmapReader& reader, MboRecord& rec) {
    if (reader.eof()) return false;
    int len;
    const char* f;

    f = reader.next_field(len);
    if (len == 0 && reader.eof()) return false;
    std::memcpy(rec.ts_recv, f, std::min(len, 31)); rec.ts_recv[std::min(len, 31)] = '\0';

    f = reader.next_field(len);
    std::memcpy(rec.ts_event, f, std::min(len, 31)); rec.ts_event[std::min(len, 31)] = '\0';

    f = reader.next_field(len); rec.rtype        = fast_atoi(f, len);
    f = reader.next_field(len); rec.publisher_id  = fast_atoi(f, len);
    f = reader.next_field(len); rec.instrument_id = fast_atoi(f, len);

    f = reader.next_field(len); rec.action = static_cast<Action>(f[0]);
    f = reader.next_field(len); rec.side   = static_cast<Side>(f[0]);

    // Price: both fixed-point and trimmed string
    f = reader.next_field(len);
    rec.price = price_to_fixed(f, len);
    trim_price_str(f, len, rec.price_str);

    f = reader.next_field(len); rec.size       = fast_atoi(f, len);
    f = reader.next_field(len); rec.channel_id = fast_atoi(f, len);
    f = reader.next_field(len); rec.order_id   = fast_atou64(f, len);
    f = reader.next_field(len); rec.flags      = fast_atoi(f, len);
    f = reader.next_field(len); rec.ts_in_delta = fast_atoi(f, len);
    f = reader.next_field(len); rec.sequence    = fast_atoi(f, len);

    f = reader.next_field(len);
    int sym_len = len;
    while (sym_len > 0 && (f[sym_len - 1] == '\r' || f[sym_len - 1] == '\n')) sym_len--;
    std::memcpy(rec.symbol, f, std::min(sym_len, 7));
    rec.symbol[std::min(sym_len, 7)] = '\0';

    reader.finish_line();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Buffered CSV writer (1 MB buffer)
// ═══════════════════════════════════════════════════════════════════════
class CsvWriter {
public:
    static constexpr size_t BUF_SIZE = 1 << 20;

    CsvWriter() : buf_(new char[BUF_SIZE]), pos_(0), fp_(nullptr) {}
    ~CsvWriter() { close(); delete[] buf_; }

    bool open(const char* path) {
        fp_ = std::fopen(path, "wb");
        if (!fp_) return false;
        std::setvbuf(fp_, nullptr, _IOFBF, BUF_SIZE);
        return true;
    }

    void close() {
        if (fp_) { flush(); std::fclose(fp_); fp_ = nullptr; }
    }

    void flush() {
        if (pos_ > 0) { std::fwrite(buf_, 1, pos_, fp_); pos_ = 0; }
    }

    void put(char c) {
        if (pos_ >= BUF_SIZE) flush();
        buf_[pos_++] = c;
    }

    void write(const char* s, size_t len) {
        if (pos_ + len >= BUF_SIZE) flush();
        if (len >= BUF_SIZE) { std::fwrite(s, 1, len, fp_); return; }
        std::memcpy(buf_ + pos_, s, len);
        pos_ += len;
    }

    void write(const char* s) { write(s, std::strlen(s)); }

    void write_int(int val) {
        char tmp[16]; int len = 0;
        if (val < 0) { tmp[len++] = '-'; val = -val; }
        if (val == 0) { tmp[len++] = '0'; }
        else {
            char d[12]; int dl = 0;
            while (val > 0) { d[dl++] = '0' + (val % 10); val /= 10; }
            for (int i = dl - 1; i >= 0; i--) tmp[len++] = d[i];
        }
        write(tmp, static_cast<size_t>(len));
    }

    void write_uint64(uint64_t val) {
        char tmp[24]; int len = 0;
        if (val == 0) { tmp[len++] = '0'; }
        else {
            char d[20]; int dl = 0;
            while (val > 0) { d[dl++] = '0' + static_cast<char>(val % 10); val /= 10; }
            for (int i = dl - 1; i >= 0; i--) tmp[len++] = d[i];
        }
        write(tmp, static_cast<size_t>(len));
    }

    // ── MBP header ────────────────────────────────────────────────
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

    // ── Write one MBP row ─────────────────────────────────────────
    void write_mbp_row(const MbpRecord& r) {
        write_int(r.row_index);
        put(',');

        // ts_recv = ts_event from MBO
        write(r.ts_event);
        put(',');

        // ts_event
        write(r.ts_event);
        put(',');

        write_int(r.rtype);     put(',');
        write_int(r.publisher_id); put(',');
        write_int(r.instrument_id); put(',');

        // action: if trade_pending, write 'T', else original
        put(r.trade_pending ? 'T' : static_cast<char>(r.action));
        put(',');

        put(static_cast<char>(r.side));
        put(',');

        write_int(r.depth);
        put(',');

        // price (trimmed string, empty if no price)
        if (r.price_str[0] != '\0') write(r.price_str);
        put(',');

        write_int(r.size);      put(',');
        write_int(r.flags);     put(',');
        write_int(r.ts_in_delta); put(',');
        write_int(r.sequence);

        // 10 depth levels: bid then ask for each depth
        for (int i = 0; i < MAX_DEPTH; i++) {
            // bid
            put(',');
            if (r.bids[i].price_str[0] != '\0') write(r.bids[i].price_str);
            put(',');
            write_int(r.bids[i].size);
            put(',');
            write_int(r.bids[i].count);

            // ask
            put(',');
            if (r.asks[i].price_str[0] != '\0') write(r.asks[i].price_str);
            put(',');
            write_int(r.asks[i].size);
            put(',');
            write_int(r.asks[i].count);
        }

        // symbol (includes leading comma in reference)
        put(',');
        write(r.symbol);

        // order_id: if trade_pending, write '0', else actual
        put(',');
        if (r.trade_pending) put('0');
        else write_uint64(r.order_id);

        put('\n');
    }

private:
    char*  buf_;
    size_t pos_;
    FILE*  fp_;
};

} // namespace obr
