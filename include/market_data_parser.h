#pragma once

#include <cstdint>

enum class MarketUpdateType : uint8_t {
    CLEAR = 1,
    ADD_ORDER = 2,
    MODIFY_ORDER = 3,
    CANCEL_ORDER = 4,
    TRADE = 5
};

struct MDPMarketUpdate {
    uint64_t timestamp;
    uint32_t sequence_num;
    uint32_t ticker_id;
    uint32_t order_id;
    uint32_t price;
    uint32_t quantity;
    MarketUpdateType type;
};

static_assert(sizeof(MDPMarketUpdate) == 32,
              "MDPMarketUpdate must be 32 bytes (natural alignment)");
