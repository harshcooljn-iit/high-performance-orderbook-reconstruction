#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace obr {

// Fixed-point price: 9 decimal places
static constexpr int64_t PRICE_SCALE = 1'000'000'000LL;
static constexpr int MAX_DEPTH       = 10;
static constexpr int MAX_LEVELS      = 12;   // reference tracks 12 levels in array

enum class Action : char {
    Add    = 'A',
    Cancel = 'C',
    Trade  = 'T',
    Fill   = 'F',
    Reset  = 'R'
};

enum class Side : char {
    Bid  = 'B',
    Ask  = 'A',
    None = 'N'
};

struct MboRecord {
    char     ts_recv[32];
    char     ts_event[32];
    int      rtype;
    int      publisher_id;
    int      instrument_id;
    Action   action;
    Side     side;
    int64_t  price;          // fixed-point
    char     price_str[24];  // original trimmed price string from CSV
    int      size;
    int      channel_id;
    uint64_t order_id;
    int      flags;
    int      ts_in_delta;
    int      sequence;
    char     symbol[8];
};

struct PriceLevel {
    int64_t price     = 0;
    char    price_str[24] = {};  // trimmed string representation
    int     size      = 0;
    int     count     = 0;

    bool empty() const { return size == 0 && count == 0; }
    void clear()       { price = 0; price_str[0] = '\0'; size = 0; count = 0; }
};

// MBP-10 output record
struct MbpRecord {
    int      row_index;
    char     ts_event[32];       // MBP uses ts_event for BOTH ts_recv and ts_event
    int      rtype;
    int      publisher_id;
    int      instrument_id;
    Action   action;             // mapped action (C in T→F→C becomes T)
    Side     side;
    int      depth;
    char     price_str[24];      // price as string
    int      size;
    int      flags;
    int      ts_in_delta;
    int      sequence;
    PriceLevel bids[MAX_DEPTH];
    PriceLevel asks[MAX_DEPTH];
    char     symbol[8];
    uint64_t order_id;
    bool     trade_pending;      // if true, action is written as 'T', order_id as '0'
};

// ── Fast price string → fixed-point ──────────────────────────────────
inline int64_t price_to_fixed(const char* s, int len) {
    if (len == 0 || s[0] == '\0') return 0;

    int64_t integer_part = 0;
    int i = 0;
    bool negative = false;

    if (s[0] == '-') { negative = true; i++; }

    while (i < len && s[i] != '.' && s[i] != '\0') {
        integer_part = integer_part * 10 + (s[i] - '0');
        i++;
    }

    uint32_t frac_part = 0;
    if (i < len && s[i] == '.') {
        i++;
        const char* frac_start = s + i;
        int dot_pos = i;
        while (i < len && s[i] != '\0') i++;
        // Parse all digits after dot as a single integer
        for (const char* p = frac_start; p < s + i; p++) {
            frac_part = frac_part * 10 + static_cast<uint32_t>(*p - '0');
        }
        // The MBO data always has 9 fractional digits
        // frac_part is already the correct value for PRICE_SCALE = 1e9
    }

    int64_t result = integer_part * PRICE_SCALE + frac_part;
    return negative ? -result : result;
}

// ── Trim trailing zeros from MBO price string ────────────────────────
// "5.510000000" → "5.51"
inline void trim_price_str(const char* src, int len, char* dst) {
    if (len == 0) { dst[0] = '\0'; return; }

    // Find the dot
    int dot_pos = -1;
    for (int i = 0; i < len; i++) {
        if (src[i] == '.') { dot_pos = i; break; }
    }

    if (dot_pos < 0) {
        (void)dot_pos;  // suppress unused warning on no-dot path
        // No dot - copy as-is
        std::memcpy(dst, src, static_cast<size_t>(len));
        dst[len] = '\0';
        return;
    }

    // Find last non-zero digit after dot
    int last_nz = len - 1;
    while (last_nz > dot_pos + 1 && src[last_nz] == '0') last_nz--;

    int out_len = last_nz + 1;
    std::memcpy(dst, src, static_cast<size_t>(out_len));
    dst[out_len] = '\0';
}

} // namespace obr
