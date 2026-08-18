#pragma once
#include "types.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace obr {

// ─── L2-fitting open-addressing hash map ──────────────────────────
// 16K entries × 12 bytes = 192KB — fits in L2 cache.
// Uses Fibonacci hashing for excellent distribution.
class FastOrderMap {
    static constexpr size_t CAP = 1 << 14; // 16384
    static constexpr size_t MASK = CAP - 1;
    struct alignas(16) Slot { uint64_t key; uint32_t size; uint32_t _pad; };
    Slot* slots;

public:
    FastOrderMap() {
        slots = static_cast<Slot*>(std::aligned_alloc(64, CAP * sizeof(Slot)));
        std::memset(slots, 0, CAP * sizeof(Slot));
    }
    ~FastOrderMap() { std::free(slots); }

    void clear() {
        std::memset(slots, 0, CAP * sizeof(Slot));
    }

    __attribute__((always_inline))
    uint32_t& operator[](uint64_t key) {
        size_t idx = (key * 11400714819323198485ull) >> (64 - 14); // top bits
        while (slots[idx].key != 0 && slots[idx].key != key) {
            idx = (idx + 1) & MASK;
        }
        if (slots[idx].key == 0) {
            slots[idx].key = key;
            slots[idx].size = 0;
        }
        return slots[idx].size;
    }

    __attribute__((always_inline))
    uint32_t* find(uint64_t key) {
        size_t idx = (key * 11400714819323198485ull) >> (64 - 14);
        while (slots[idx].key != 0) {
            if (slots[idx].key == key) return &slots[idx].size;
            idx = (idx + 1) & MASK;
        }
        return nullptr;
    }

    // Prefetch the likely slot for a future lookup
    __attribute__((always_inline))
    void prefetch(uint64_t key) const {
        size_t idx = (key * 11400714819323198485ull) >> (64 - 14);
        __builtin_prefetch(&slots[idx], 0, 1); // read, low temporal locality
    }
};

// ─── Orderbook: maintains sorted bid/ask levels ───────────────────
// Uses static arrays of 12 levels + overflow arrays of 256.
// Key change: PriceLevel no longer maintains pre-formatted strings.
// Formatting happens LAZILY only when output is produced.
class Orderbook {
public:
    Orderbook() { reset(); }

    void reset() {
        for (int i = 0; i < MAX_LEVELS; i++) { bids_[i].clear(); asks_[i].clear(); }
        n_bid_ = 0;
        n_ask_ = 0;
        orders_.clear();
        n_asks_map_ = 0;
        n_bids_map_ = 0;
        depth_ = 0;
        trade_pending_ = false;
        is_bid_level_changed_ = false;
    }

    __attribute__((hot))
    bool apply(const MboRecord& mbo) {
        depth_ = 0;

        switch (static_cast<char>(mbo.action)) {
        case 'A':
            if (mbo.side == Side::Bid)      add_bid(mbo);
            else if (mbo.side == Side::Ask) add_ask(mbo);
            break;
        case 'C':
            if (mbo.side == Side::Bid)      cancel_bid(mbo);
            else if (mbo.side == Side::Ask) cancel_ask(mbo);
            break;
        case 'T':
            if (mbo.side != Side::None) trade_pending_ = true;
            break;
        default:
            break;
        }

        if (__builtin_expect(depth_ >= 12, 0))                return false;
        if (trade_pending_ && mbo.action != Action::Cancel)    return false;
        if (__builtin_expect(depth_ == 11, 0) &&
            !(mbo.action == Action::Cancel &&
              mbo.side == Side::Bid &&
              is_bid_level_changed_))                          return false;

        return true;
    }

    void reset_trade() { trade_pending_ = false; }
    bool trade_pending() const { return trade_pending_; }
    int  depth() const { return depth_; }

    // ── Direct-to-buffer formatting ───────────────────────────────
    // Writes bid/ask levels directly into the output buffer.
    // Returns pointer past end of written data.
    __attribute__((hot))
    char* write_levels(char* __restrict__ p) const {
        int bd = std::min(n_bid_, 10);
        int ad = std::min(n_ask_, 10);

        for (int i = 0; i < 10; i++) {
            if (i < bd) p = write_level_ptr(p, bids_[i]);
            else        p = write_empty_level(p);
            if (i < ad) p = write_level_ptr(p, asks_[i]);
            else        p = write_empty_level(p);
        }
        return p;
    }

private:
    PriceLevel bids_[MAX_LEVELS];
    PriceLevel asks_[MAX_LEVELS];
    int n_bid_ = 0;
    int n_ask_ = 0;
    int depth_ = 0;
    bool trade_pending_ = false;
    bool is_bid_level_changed_ = false;

    FastOrderMap orders_;

    PriceLevel asks_map_[256];
    int n_asks_map_ = 0;
    PriceLevel bids_map_[256];
    int n_bids_map_ = 0;

    __attribute__((always_inline))
    void add_ask(const MboRecord& mbo) {
        uint32_t& sz = orders_[mbo.order_id];
        bool inc_count = (sz == 0);
        sz += mbo.size;

        int limit = std::min(n_ask_, 12);
        if (n_ask_ < 12 || mbo.price <= asks_[11].price) {
            int i = 0;
            for (; i < limit; i++) {
                if (mbo.price <= asks_[i].price) break;
            }
            depth_ = i;

            if (i < n_ask_ && mbo.price == asks_[i].price) {
                asks_[i].size += mbo.size;
                if (inc_count) asks_[i].count++;
                asks_[i].reformat();
            } else {
                if (n_ask_ == 12) {
                    insert_ask_map(asks_[11]);
                } else {
                    n_ask_++;
                }
                for (int j = 10; j >= i; j--) asks_[j + 1] = asks_[j];
                asks_[i].init(mbo.price, mbo.size, mbo.price_str, mbo.price_len);
            }
        } else {
            add_ask_map(mbo, inc_count);
            depth_ = 12;
        }
    }

    __attribute__((always_inline))
    void add_bid(const MboRecord& mbo) {
        uint32_t& sz = orders_[mbo.order_id];
        bool inc_count = (sz == 0);
        sz += mbo.size;

        int limit = std::min(n_bid_, 12);
        if (n_bid_ < 12 || mbo.price >= bids_[11].price) {
            int i = 0;
            for (; i < limit; i++) {
                if (mbo.price >= bids_[i].price) break;
            }
            depth_ = i;

            if (i < n_bid_ && mbo.price == bids_[i].price) {
                is_bid_level_changed_ = false;
                bids_[i].size += mbo.size;
                if (inc_count) bids_[i].count++;
                bids_[i].reformat();
            } else {
                is_bid_level_changed_ = true;
                if (n_bid_ == 12) {
                    insert_bid_map(bids_[11]);
                } else {
                    n_bid_++;
                }
                for (int j = 10; j >= i; j--) bids_[j + 1] = bids_[j];
                bids_[i].init(mbo.price, mbo.size, mbo.price_str, mbo.price_len);
            }
        } else {
            add_bid_map(mbo, inc_count);
            depth_ = 12;
        }
    }

    __attribute__((always_inline))
    void cancel_ask(const MboRecord& mbo) {
        bool dec_count = false;
        uint32_t* sz = orders_.find(mbo.order_id);
        if (sz) {
            *sz -= mbo.size;
            if (*sz == 0) dec_count = true;
        } else {
            orders_[mbo.order_id] = 0;
        }

        if (n_asks_map_ == 0 || mbo.price <= asks_[11].price) {
            int i = 0;
            int limit = std::min(n_ask_, 12);
            for (; i < limit; i++) {
                if (mbo.price <= asks_[i].price) break;
            }
            depth_ = i;

            if (i < n_ask_ && mbo.price == asks_[i].price) {
                asks_[i].size -= mbo.size;
                if (dec_count) asks_[i].count--;
                if (asks_[i].size <= 0) {
                    for (; i < std::min(n_ask_, 12) - 1; i++) asks_[i] = asks_[i + 1];
                    if (n_ask_ == 12 && n_asks_map_ > 0) {
                        asks_[11] = asks_map_[0];
                        for (int j = 0; j < n_asks_map_ - 1; j++) asks_map_[j] = asks_map_[j+1];
                        n_asks_map_--;
                    } else {
                        n_ask_--;
                    }
                } else {
                    asks_[i].reformat();
                }
            }
        } else {
            cancel_ask_map(mbo, dec_count);
            depth_ = 12;
        }
    }

    __attribute__((always_inline))
    void cancel_bid(const MboRecord& mbo) {
        bool dec_count = false;
        uint32_t* sz = orders_.find(mbo.order_id);
        if (sz) {
            *sz -= mbo.size;
            if (*sz == 0) dec_count = true;
        } else {
            orders_[mbo.order_id] = 0;
        }

        if (n_bid_ < 12 || mbo.price >= bids_[11].price) {
            int i = 0;
            int limit = std::min(n_bid_, 12);
            for (; i < limit; i++) {
                if (mbo.price >= bids_[i].price) break;
            }
            depth_ = i;

            if (i < n_bid_ && mbo.price == bids_[i].price) {
                bids_[i].size -= mbo.size;
                if (dec_count) bids_[i].count--;
                if (bids_[i].size <= 0) {
                    is_bid_level_changed_ = true;
                    for (; i < std::min(n_bid_, 12) - 1; i++) bids_[i] = bids_[i + 1];
                    if (n_bid_ == 12 && n_bids_map_ > 0) {
                        bids_[11] = bids_map_[0];
                        for (int j = 0; j < n_bids_map_ - 1; j++) bids_map_[j] = bids_map_[j+1];
                        n_bids_map_--;
                    } else {
                        n_bid_--;
                    }
                } else {
                    is_bid_level_changed_ = false;
                    bids_[i].reformat();
                }
            }
        } else {
            cancel_bid_map(mbo, dec_count);
            depth_ = 12;
        }
    }

    // ── Overflow map operations (cold path) ───────────────────────
    void insert_ask_map(const PriceLevel& lv) {
        int i = 0;
        while (i < n_asks_map_ && asks_map_[i].price < lv.price) i++;
        for (int j = n_asks_map_; j > i; j--) asks_map_[j] = asks_map_[j - 1];
        asks_map_[i] = lv;
        n_asks_map_++;
    }

    void insert_bid_map(const PriceLevel& lv) {
        int i = 0;
        while (i < n_bids_map_ && bids_map_[i].price > lv.price) i++;
        for (int j = n_bids_map_; j > i; j--) bids_map_[j] = bids_map_[j - 1];
        bids_map_[i] = lv;
        n_bids_map_++;
    }

    void add_ask_map(const MboRecord& mbo, bool inc_count) {
        int i = 0;
        while (i < n_asks_map_ && asks_map_[i].price < mbo.price) i++;
        if (i < n_asks_map_ && asks_map_[i].price == mbo.price) {
            asks_map_[i].size += mbo.size;
            if (inc_count) asks_map_[i].count++;
            asks_map_[i].reformat();
        } else {
            for (int j = n_asks_map_; j > i; j--) asks_map_[j] = asks_map_[j - 1];
            asks_map_[i].init(mbo.price, mbo.size, mbo.price_str, mbo.price_len);
            n_asks_map_++;
        }
    }

    void add_bid_map(const MboRecord& mbo, bool inc_count) {
        int i = 0;
        while (i < n_bids_map_ && bids_map_[i].price > mbo.price) i++;
        if (i < n_bids_map_ && bids_map_[i].price == mbo.price) {
            bids_map_[i].size += mbo.size;
            if (inc_count) bids_map_[i].count++;
            bids_map_[i].reformat();
            is_bid_level_changed_ = false;
        } else {
            for (int j = n_bids_map_; j > i; j--) bids_map_[j] = bids_map_[j - 1];
            bids_map_[i].init(mbo.price, mbo.size, mbo.price_str, mbo.price_len);
            n_bids_map_++;
            is_bid_level_changed_ = true;
        }
    }

    void cancel_ask_map(const MboRecord& mbo, bool dec_count) {
        int i = 0;
        while (i < n_asks_map_ && asks_map_[i].price < mbo.price) i++;
        if (i < n_asks_map_ && asks_map_[i].price == mbo.price) {
            asks_map_[i].size -= mbo.size;
            if (dec_count) asks_map_[i].count--;
            if (asks_map_[i].size <= 0) {
                for (int j = i; j < n_asks_map_ - 1; j++) asks_map_[j] = asks_map_[j+1];
                n_asks_map_--;
            } else {
                asks_map_[i].reformat();
            }
        }
    }

    void cancel_bid_map(const MboRecord& mbo, bool dec_count) {
        int i = 0;
        while (i < n_bids_map_ && bids_map_[i].price > mbo.price) i++;
        if (i < n_bids_map_ && bids_map_[i].price == mbo.price) {
            bids_map_[i].size -= mbo.size;
            if (dec_count) bids_map_[i].count--;
            if (bids_map_[i].size <= 0) {
                for (int j = i; j < n_bids_map_ - 1; j++) bids_map_[j] = bids_map_[j+1];
                n_bids_map_--;
                is_bid_level_changed_ = true;
            } else {
                bids_map_[i].reformat();
                is_bid_level_changed_ = false;
            }
        }
    }
};

} // namespace obr
