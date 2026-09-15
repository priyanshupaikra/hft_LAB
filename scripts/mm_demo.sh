#!/usr/bin/env bash
# Market-making demo: gateway + MD stream + a quoting bot + noise traders.
# Watch the mm's PnL accumulate from the spread while its inventory
# oscillates (skewed quotes bleed it back toward zero).
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-5570}"
MD_PORT="${MD_PORT:-7600}"
DURATION="${DURATION:-14}"

echo "=== build + tests =================================================="
make -s
./build/hft_tests | tail -1

echo
echo "=== gateway: port=$PORT md_port=$MD_PORT ==========================="
./build/hft_gateway --port "$PORT" --md-port "$MD_PORT" --rate 100000 --burst 200000 2>/dev/null &
GATEWAY_PID=$!
trap 'kill $GATEWAY_PID $MD_PID $MM_PID 2>/dev/null || true' EXIT
sleep 0.5

echo "=== market-data client (gap detection via SNAP) ===================="
python3 py/md_client.py --md-port "$MD_PORT" --port "$PORT" --quiet &
MD_PID=$!
sleep 0.3

echo "=== market maker + noise traders (${DURATION}s) ===================="
python3 py/mm/mm.py --port "$PORT" --duration "$DURATION" &
MM_PID=$!
sleep 1.5   # let the mm get its first quotes up

# Noise traders hit the quotes with random-side market orders.
python3 py/injector/injector.py --port "$PORT" --token dev-beta-token \
  --rate 15 --duration "$DURATION" --conns 2 --noise

wait "$MM_PID"
echo
echo "=== final STATS ===================================================="
python3 py/monitor/monitor.py --port "$PORT" --rounds 1
kill "$MD_PID" 2>/dev/null || true
echo "(md_client ran quiet; rerun without --quiet to watch the tick stream)"
