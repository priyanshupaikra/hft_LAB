#!/usr/bin/env bash
# End-to-end hft-lab demo: unit tests -> live gateway -> normal flow ->
# rate-limiter trip -> kill-switch trip (auto, via fat fingers) -> final stats.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-5555}"
GATEWAY="build/hft_gateway"
MONITOR="python3 py/monitor/monitor.py"
INJECT="python3 py/injector/injector.py"

echo "=== 0) unit tests + build ========================================="
make -s
./build/hft_tests | tail -1

echo
echo "=== starting gateway: port=$PORT, alpha rate limit 400/s burst 800 ==="
"$GATEWAY" --port "$PORT" --rate 400 --burst 800 2>/dev/null &
GATEWAY_PID=$!
trap 'kill $GATEWAY_PID 2>/dev/null || true' EXIT
sleep 0.5

echo
echo "=== 1) normal flow: 300 orders/s + 2% dup retries + rare fat finger ==="
$INJECT --port "$PORT" --rate 300 --duration 3 --dups 0.02 --fatfinger 0.002

echo
echo "=== 2) RATE LIMITER: 2 conns x 1000/s = 2000/s against a 400/s limit ==="
# Each connection is its own session -> its own bucket (400/s refill, 800
# burst => ~2000 order capacity per session over 3s). Sending 3000/session
# exhausts it and the tail gets RATE_LIMITED.
$INJECT --port "$PORT" --rate 1000 --duration 3 --conns 2

echo
echo "=== 3) KILL SWITCH: 50% fat fingers -> risk rejects trip the breaker ==="
$INJECT --port "$PORT" --rate 200 --duration 4 --fatfinger 0.5 --token dev-alpha-token

echo
echo "=== 4) final STATS (admin view) ===================================="
$MONITOR --port "$PORT" --rounds 1

echo
echo "demo done. kill switch state above should show OPEN (it tripped in step 3)."
