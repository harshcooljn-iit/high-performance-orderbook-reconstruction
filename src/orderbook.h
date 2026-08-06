#pragma once
#include "types.h"
#include <map>
#include <unordered_map>
#include <cstring>
#include <algorithm>

namespace obr {

class Orderbook {
public:
    Orderbook() { reset(); }

    void reset() {
        for (int i = 0; i < MAX_LEVELS; i++) { bids_[i].clear(); asks_[i].clear(); }
        n_bid_ = 0;
        n_ask_ = 0;
        orders_.clear();
        orders_.reserve(4096);
        asks_map_.clear();
        bids_map_.clear();
        depth_ = 0;
        trade_pending_ = false;
        is_bid_level_changed_ = false;
    }

    // ── Apply an MBO event to the book ────────────────────────────
    // Returns true if this event should produce an MBP output row
    bool apply(MboRecord& mbo) {
        depth_ = 0;

        if (mbo.action == Action::Add) {
            if (mbo.side == Side::Bid)      add_bid(mbo);
            else if (mbo.side == Side::Ask) add_ask(mbo);
        }
        else if (mbo.action == Action::Cancel) {
            if (mbo.side == Side::Bid)      cancel_bid(mbo);
            else if (mbo.side == Side::Ask) cancel_ask(mbo);
        }
        else if (mbo.action == Action::Trade) {
            if (mbo.side != Side::None) {
                trade_pending_ = true;
            }
        }

        // Filtering rules (from reference):
        if (depth_ >= 12)                                  return false;
        if (trade_pending_ && mbo.action != Action::Cancel) return false;
        if (depth_ == 11 && !(mbo.action == Action::Cancel &&
                              mbo.side == Side::Bid &&
                              is_bid_level_changed_))       return false;

        return true;
    }

    void reset_trade() { trade_pending_ = false; }
    bool trade_pending() const { return trade_pending_; }
    int  depth() const { return depth_; }

    // ── Snapshot top-10 ────────────────────────────────────────────
    void snapshot(PriceLevel* out_bids, PriceLevel* out_asks) const {
        int bd = std::min(n_bid_, MAX_DEPTH);
        int ad = std::min(n_ask_, MAX_DEPTH);
        for (int i = 0; i < bd; i++) out_bids[i] = bids_[i];
        for (int i = bd; i < MAX_DEPTH; i++) out_bids[i].clear();
        for (int i = 0; i < ad; i++) out_asks[i] = asks_[i];
        for (int i = ad; i < MAX_DEPTH; i++) out_asks[i].clear();
    }

    int n_bid() const { return n_bid_; }
    int n_ask() const { return n_ask_; }

private:
    PriceLevel bids_[MAX_LEVELS];       // descending by price
    PriceLevel asks_[MAX_LEVELS];       // ascending  by price
    int n_bid_ = 0;
    int n_ask_ = 0;
    int depth_ = 0;
    bool trade_pending_ = false;
    bool is_bid_level_changed_ = false;

    // order_id → remaining size (for count tracking)
    std::unordered_map<uint64_t, uint32_t> orders_;

    // Overflow maps: sorted by price (asks ascending, bids descending)
    std::map<int64_t, PriceLevel> asks_map_;
    std::map<int64_t, PriceLevel, std::greater<int64_t>> bids_map_;

    // ── Add Ask ───────────────────────────────────────────────────
    void add_ask(MboRecord& mbo) {
        bool inc_count = false;
        auto it = orders_.find(mbo.order_id);
        if (it != orders_.end()) {
            it->second += static_cast<uint32_t>(mbo.size);
        } else {
            orders_[mbo.order_id] = static_cast<uint32_t>(mbo.size);
            inc_count = true;
        }

        // Check if price fits in top 12
        if (n_ask_ < 12 || mbo.price <= asks_[11].price) {
            int i = 0;
            for (; i < std::min(n_ask_, 12); i++) {
                if (mbo.price <= asks_[i].price) break;
            }
            depth_ = i;

            if (i < n_ask_ && mbo.price == asks_[i].price) {
                // Existing level
                asks_[i].size += mbo.size;
                if (inc_count) asks_[i].count++;
            } else {
                // New level - push overflow to map
                if (n_ask_ == 12) {
                    asks_map_[asks_[11].price] = asks_[11];
                } else {
                    n_ask_++;
                }
                // Shift down
                for (int j = 10; j >= i; j--) asks_[j + 1] = asks_[j];
                // Insert
                asks_[i].price = mbo.price;
                std::memcpy(asks_[i].price_str, mbo.price_str, sizeof(asks_[i].price_str));
                asks_[i].size = mbo.size;
                asks_[i].count = 1;
            }
        } else {
            // Goes into overflow map
            if (asks_map_.count(mbo.price)) {
                asks_map_[mbo.price].size += mbo.size;
                if (inc_count) asks_map_[mbo.price].count++;
            } else {
                PriceLevel lv;
                lv.price = mbo.price;
                std::memcpy(lv.price_str, mbo.price_str, sizeof(lv.price_str));
                lv.size = mbo.size;
                lv.count = 1;
                asks_map_[mbo.price] = lv;
            }
            depth_ = 12;
        }
    }

    // ── Add Bid ───────────────────────────────────────────────────
    void add_bid(MboRecord& mbo) {
        bool inc_count = false;
        auto it = orders_.find(mbo.order_id);
        if (it != orders_.end()) {
            it->second += static_cast<uint32_t>(mbo.size);
        } else {
            orders_[mbo.order_id] = static_cast<uint32_t>(mbo.size);
            inc_count = true;
        }

        if (n_bid_ < 12 || mbo.price >= bids_[11].price) {
            int i = 0;
            for (; i < std::min(n_bid_, 12); i++) {
                if (mbo.price >= bids_[i].price) break;
            }
            depth_ = i;

            if (i < n_bid_ && mbo.price == bids_[i].price) {
                is_bid_level_changed_ = false;
                bids_[i].size += mbo.size;
                if (inc_count) bids_[i].count++;
            } else {
                is_bid_level_changed_ = true;
                if (n_bid_ == 12) {
                    bids_map_[bids_[11].price] = bids_[11];
                } else {
                    n_bid_++;
                }
                for (int j = 10; j >= i; j--) bids_[j + 1] = bids_[j];
                bids_[i].price = mbo.price;
                std::memcpy(bids_[i].price_str, mbo.price_str, sizeof(bids_[i].price_str));
                bids_[i].size = mbo.size;
                bids_[i].count = 1;
            }
        } else {
            if (bids_map_.count(mbo.price)) {
                is_bid_level_changed_ = false;
                bids_map_[mbo.price].size += mbo.size;
                if (inc_count) bids_map_[mbo.price].count++;
            } else {
                is_bid_level_changed_ = true;
                PriceLevel lv;
                lv.price = mbo.price;
                std::memcpy(lv.price_str, mbo.price_str, sizeof(lv.price_str));
                lv.size = mbo.size;
                lv.count = 1;
                bids_map_[mbo.price] = lv;
            }
            depth_ = 12;
        }
    }

    // ── Cancel Ask ────────────────────────────────────────────────
    void cancel_ask(MboRecord& mbo) {
        bool dec_count = false;
        auto it = orders_.find(mbo.order_id);
        if (it != orders_.end()) {
            it->second -= static_cast<uint32_t>(mbo.size);
            if (static_cast<int32_t>(it->second) <= 0) {
                orders_.erase(it);
                dec_count = true;
            }
        } else {
            orders_[mbo.order_id] = 0;
        }

        if (asks_map_.empty() || mbo.price <= asks_[11].price) {
            int i = 0;
            for (; i < std::min(n_ask_, 12); i++) {
                if (mbo.price <= asks_[i].price) break;
            }
            depth_ = i;

            if (mbo.price == asks_[i].price) {
                asks_[i].size -= mbo.size;
                if (dec_count) asks_[i].count--;

                if (asks_[i].size <= 0) {
                    // Remove level, shift up
                    for (; i < std::min(n_ask_, 12) - 1; i++) {
                        asks_[i] = asks_[i + 1];
                    }
                    if (n_ask_ == 12 && !asks_map_.empty()) {
                        asks_[11] = asks_map_.begin()->second;
                        asks_map_.erase(asks_map_.begin());
                    } else {
                        n_ask_--;
                    }
                }
            }
            // else: level not found (shouldn't happen with valid data)
        } else {
            asks_map_[mbo.price].size -= mbo.size;
            if (dec_count) asks_map_[mbo.price].count--;
            if (asks_map_[mbo.price].size <= 0) {
                asks_map_.erase(mbo.price);
            }
            depth_ = 12;
        }
    }

    // ── Cancel Bid ────────────────────────────────────────────────
    void cancel_bid(MboRecord& mbo) {
        bool dec_count = false;
        auto it = orders_.find(mbo.order_id);
        if (it != orders_.end()) {
            it->second -= static_cast<uint32_t>(mbo.size);
            if (static_cast<int32_t>(it->second) <= 0) {
                orders_.erase(it);
                dec_count = true;
            }
        } else {
            orders_[mbo.order_id] = 0;
        }

        if (n_bid_ < 12 || mbo.price >= bids_[11].price) {
            int i = 0;
            for (; i < std::min(n_bid_, 12); i++) {
                if (mbo.price >= bids_[i].price) break;
            }
            depth_ = i;

            if (mbo.price == bids_[i].price) {
                bids_[i].size -= mbo.size;
                if (dec_count) bids_[i].count--;

                if (bids_[i].size <= 0) {
                    is_bid_level_changed_ = true;
                    for (; i < std::min(n_bid_, 12) - 1; i++) {
                        bids_[i] = bids_[i + 1];
                    }
                    if (n_bid_ == 12 && !bids_map_.empty()) {
                        bids_[11] = bids_map_.begin()->second;
                        bids_map_.erase(bids_map_.begin());
                    } else {
                        n_bid_--;
                    }
                } else {
                    is_bid_level_changed_ = false;
                }
            }
        } else {
            bids_map_[mbo.price].size -= mbo.size;
            if (dec_count) bids_map_[mbo.price].count--;
            if (bids_map_[mbo.price].size <= 0) {
                bids_map_.erase(mbo.price);
                is_bid_level_changed_ = true;
            } else {
                is_bid_level_changed_ = false;
            }
            depth_ = 12;
        }
    }
};

} // namespace obr
