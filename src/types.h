#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace obr {

static constexpr int64_t PRICE_SCALE = 1'000'000'000LL;
static constexpr int MAX_DEPTH       = 10;
static constexpr int MAX_LEVELS      = 12;

enum class Action : char {
    Add    = 'A',
    Cancel = 'C',
    Modify = 'M',
    Trade  = 'T',
    Fill   = 'F',
    Reset  = 'R',
    EOF_MARKER = '\0'
};

enum class Side : char {
    Bid  = 'B',
    Ask  = 'A',
    None = 'N'
};

// ─── Compact MBO record: pointers into mmap'd buffer ──────────────
struct MboRecord {
    const char* raw;
    
    uint16_t ts_event_idx;
    uint16_t publisher_idx;
    uint16_t action_idx;
    
    Action   action;
    Side     side;
    
    int64_t  price;
    const char* price_str;
    uint16_t price_len;
    
    int      size;
    const char* size_str;
    uint16_t size_len;
    
    uint64_t order_id;
    const char* order_id_str;
    uint16_t order_id_len;
    
    const char* flags_str;
    uint16_t flags_len;
    
    const char* symbol_str;
    uint16_t symbol_len;
};

// ─── Fast itoa: branchless for 1-4 digit numbers ──────────────────
// Most orderbook sizes/counts are 1-9999 range.
__attribute__((always_inline))
inline int fast_itoa(int val, char* buf) {
    if (__builtin_expect(val == 0, 0)) { buf[0] = '0'; return 1; }
    unsigned u = static_cast<unsigned>(val);
    // Specialize for common small values
    if (u < 10) { buf[0] = '0' + u; return 1; }
    if (u < 100) { buf[0] = '0' + u / 10; buf[1] = '0' + u % 10; return 2; }
    if (u < 1000) { buf[0] = '0' + u / 100; buf[1] = '0' + (u / 10) % 10; buf[2] = '0' + u % 10; return 3; }
    // General case for larger values
    char tmp[12]; int len = 0;
    while (u > 0) { tmp[len++] = '0' + (u % 10); u /= 10; }
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    return len;
}

// ─── Price level with pre-formatted output string ─────────────────
// The fmt string (",price,size,count") is maintained incrementally:
// only updated when size or count actually changes.
struct PriceLevel {
    int64_t price;
    int     size;
    int     count;
    char    price_str[24];
    uint8_t price_len;
    
    // Pre-formatted output: ",price,size,count"
    char    fmt[48];
    uint8_t fmt_len;

    bool empty() const { return size == 0 && count == 0; }
    void clear()       { price = 0; size = 0; count = 0; fmt_len = 0; }

    __attribute__((always_inline))
    void init(int64_t px, int sz, const char* px_str, uint8_t px_len) {
        price = px;
        size = sz;
        count = 1;
        price_len = px_len;
        std::memcpy(price_str, px_str, px_len);
        reformat();
    }

    // Only reformats size+count portion, reuses price portion
    __attribute__((always_inline))
    void reformat() {
        fmt[0] = ',';
        std::memcpy(fmt + 1, price_str, price_len);
        int pos = 1 + price_len;
        fmt[pos++] = ',';
        pos += fast_itoa(size, fmt + pos);
        fmt[pos++] = ',';
        pos += fast_itoa(count, fmt + pos);
        fmt_len = pos;
    }
};

// ─── Write a pre-formatted level directly into output ─────────────
__attribute__((always_inline))
inline char* write_level_ptr(char* __restrict__ p, const PriceLevel& lv) {
    std::memcpy(p, lv.fmt, lv.fmt_len);
    return p + lv.fmt_len;
}

__attribute__((always_inline))
inline char* write_empty_level(char* __restrict__ p) {
    std::memcpy(p, ",,0,0", 5);
    return p + 5;
}

// ─── Fused price parse: fixed-point + trim in single pass ─────────
inline int64_t price_to_fixed(const char* s, int len) {
    if (__builtin_expect(len == 0, 0)) return 0;
    int64_t integer_part = 0;
    int i = 0;
    bool negative = false;
    if (s[0] == '-') { negative = true; i++; }
    while (i < len && s[i] != '.') {
        integer_part = integer_part * 10 + (s[i] - '0');
        i++;
    }
    uint32_t frac_part = 0;
    if (i < len && s[i] == '.') {
        i++;
        for (int j = i; j < len; j++) {
            frac_part = frac_part * 10 + static_cast<uint32_t>(s[j] - '0');
        }
    }
    int64_t result = integer_part * PRICE_SCALE + frac_part;
    return negative ? -result : result;
}

inline void trim_price_str(const char* src, int len, const char*& out_str, uint16_t& out_len) {
    int dot_pos = -1;
    for (int i = 0; i < len; i++) {
        if (src[i] == '.') { dot_pos = i; break; }
    }
    if (dot_pos < 0) {
        out_str = src;
        out_len = len;
        return;
    }
    int last_nz = len - 1;
    while (last_nz > dot_pos + 1 && src[last_nz] == '0') last_nz--;
    out_str = src;
    out_len = last_nz + 1;
}

// Fused: parse price to fixed-point AND trim string in one pass
__attribute__((always_inline))
inline void parse_price_fused(const char* s, int len,
                               int64_t& price_out,
                               const char*& str_out, uint16_t& str_len_out) {
    if (__builtin_expect(len == 0, 0)) {
        price_out = 0;
        str_out = s;
        str_len_out = 0;
        return;
    }
    int64_t integer_part = 0;
    int i = 0;
    bool negative = false;
    if (s[0] == '-') { negative = true; i++; }
    while (i < len && s[i] != '.') {
        integer_part = integer_part * 10 + (s[i] - '0');
        i++;
    }
    uint32_t frac_part = 0;
    int trim_end = len;
    if (i < len && s[i] == '.') {
        int dot_pos = i;
        i++;
        for (int j = i; j < len; j++) {
            frac_part = frac_part * 10 + static_cast<uint32_t>(s[j] - '0');
        }
        trim_end = len - 1;
        while (trim_end > dot_pos + 1 && s[trim_end] == '0') trim_end--;
        trim_end++;
    }
    price_out = negative ? -(integer_part * PRICE_SCALE + frac_part) : (integer_part * PRICE_SCALE + frac_part);
    str_out = s;
    str_len_out = trim_end;
}

} // namespace obr
