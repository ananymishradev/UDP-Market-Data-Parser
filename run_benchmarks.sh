#!/usr/bin/env bash
set -euo pipefail
trap 'kill 0' EXIT

BUILD_DIR="$(dirname "$0")/build"
CONSUMER="$BUILD_DIR/market_data_consumer"
GENERATOR="$BUILD_DIR/mock_feed_generator"

CONSUMER_PORT=20013
CONSUMER_CORE=3
GENERATOR_CORE=5
RATE=500000
WARMUP=10000

echo "=============================================="
echo " Market Data Parser — Benchmark Suite"
echo "=============================================="
echo ""

if ! command -v taskset &>/dev/null; then
    echo "[ERROR] taskset not found — cannot pin cores."
    echo "        Install util-linux or run on Linux."
    exit 1
fi

echo "[build] Configuring..."
cmake -S "$(dirname "$0")" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -1
echo "[build] Compiling..."
cmake --build "$BUILD_DIR" -- -j"$(nproc)" 2>&1 | tail -1

echo ""
echo "[info] Consumer pinned to Core $CONSUMER_CORE"
echo "[info] Generator pinned to Core $GENERATOR_CORE"
echo "[info] Generator rate: $RATE pkts/sec"
echo "[info] Warmup: $WARMUP packets"
echo ""

run_config() {
    local label="$1"
    local batch_flag="$2"
    local core="$3"

    echo "----------------------------------------------"
    echo " Configuration: $label"
    echo "----------------------------------------------"

    ARGS="--benchmark --port $CONSUMER_PORT --core $core --warmup $WARMUP $batch_flag"

    # Start consumer in background
    taskset -c "$core" "$CONSUMER" $ARGS &
    CONSUMER_PID=$!

    sleep 0.3

    # Start generator for fixed duration
    taskset -c "$GENERATOR_CORE" "$GENERATOR" \
        --port "$CONSUMER_PORT" \
        --rate "$RATE" &
    GENERATOR_PID=$!

    # Let it run for N packets (~2 seconds at 500k/s)
    sleep 2

    kill "$GENERATOR_PID" 2>/dev/null || true
    wait "$GENERATOR_PID" 2>/dev/null || true

    kill -INT "$CONSUMER_PID" 2>/dev/null || true
    wait "$CONSUMER_PID" 2>/dev/null || true

    echo ""
    echo ""
}

# ── Run all configurations ──────────────────────────────────

# 1. Baseline: no batch, Core 0 (noisy)
run_config "Baseline — Core 0, no batch" "" "0"

# 2. Isolated core, no batch
run_config "Isolated core — Core $CONSUMER_CORE, no batch" "" "$CONSUMER_CORE"

# 3. Isolated core + batch
run_config "Optimized — Core $CONSUMER_CORE + batch" "--batch" "$CONSUMER_CORE"

echo "=============================================="
echo " All configurations complete."
echo " Compare the Max Latency and tail percentiles"
echo " above to see the impact of core isolation"
echo " and recvmmsg batching."
echo "=============================================="
