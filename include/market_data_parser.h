#pragma once

#include <cstdint>

// wire types, kept small so struct stays 32 bytes
enum class MarketUpdateType : uint8_t {
    CLEAR = 1,
    ADD_ORDER = 2,
    MODIFY_ORDER = 3,
    CANCEL_ORDER = 4,
    TRADE = 5
};

// this layout is the wire format. do not reorder.
// u64 first for alignment, then u32s, then u8.
// took a couple tries to get it to 32 exactly.
struct MDPMarketUpdate {
    uint64_t timestamp;
    uint32_t sequence_num;
    uint32_t ticker_id;
    uint32_t order_id;
    uint32_t price;
    uint32_t quantity;
    MarketUpdateType type;
    // compiler adds 3 pad bytes here, that is fine
};

static_assert(sizeof(MDPMarketUpdate) == 32,
              "MDPMarketUpdate must be 32 bytes (natural alignment)");
