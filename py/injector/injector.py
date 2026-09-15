#!/usr/bin/env python3
"""hft-lab load injector: paced order flow against the gateway.

Drives the wire protocol (AUTH / NEW) at a target rate and reports what came
back: fills vs rejections broken out by reason (RATE_LIMITED, DUPLICATE,
RISK_*, KILL_SWITCH) plus client-measured RTT percentiles. Used by
scripts/demo.sh to trip the rate limiter and the kill switch on purpose.

  python3 py/injector/injector.py --rate 1200 --duration 3
"""
import argparse
import asyncio
import random
import time

NS = time.perf_counter_ns


class Summary:
    def __init__(self):
        self.counts = {}
        self.rtts = []

    def add(self, kind, rtt_ns=None):
        self.counts[kind] = self.counts.get(kind, 0) + 1
        if rtt_ns is not None:
            self.rtts.append(rtt_ns)


def percentile(sorted_vals, q):
    if not sorted_vals:
        return 0
    idx = min(len(sorted_vals) - 1, int(q / 100.0 * len(sorted_vals)))
    return sorted_vals[idx]


async def run_conn(idx: int, args, stop_at_ns: int, summary: Summary) -> int:
    reader, writer = await asyncio.open_connection(args.host, args.port)

    async def rpc(line: str):
        t0 = NS()
        writer.write((line + "\n").encode())
        await writer.drain()
        resp = (await reader.readline()).decode().strip()
        return resp, NS() - t0

    resp, _ = await rpc(f"AUTH {args.token}")
    if not resp.startswith("OK"):
        raise RuntimeError(f"auth failed: {resp}")

    rng = random.Random(10_000 + idx)
    cl_base = idx * 1_000_000_000
    sent = 0
    interval_ns = 1e9 / args.rate
    next_send_ns = NS()

    while True:
        now = NS()
        if now >= stop_at_ns:
            break
        if now < next_send_ns:
            await asyncio.sleep((next_send_ns - now) / 1e9)
            now = NS()
            if now >= stop_at_ns:
                break
        next_send_ns = max(next_send_ns + interval_ns, now)

        sent += 1
        prev_cl = cl_base + sent - 1
        is_dup = sent > 1 and rng.random() < args.dups
        cl = prev_cl if is_dup else cl_base + sent

        if args.noise:
            # Noise trader: random-side market orders that lift whatever is
            # quoted — the flow a market maker earns the spread from.
            side = rng.choice("BS")
            qty, px = rng.randint(1, 20), 0  # price 0 => market (IOC)
        else:
            side = rng.choice("BS")
            if rng.random() < args.fatfinger:
                # Fat finger: sane qty, insane price -> RISK_NOTIONAL (qty cap
                # untouched, so this exercises the notional band specifically).
                qty, px = 100, 9_999_999  # $99,999.99/share: 100x fair value
            else:
                qty = rng.randint(1, 100)
                px = 10_000 + rng.randint(-500, 500)  # ~$100.00 +/- $5, in cents

        resp, rtt = await rpc(f"NEW {cl} {side} {qty} {px}")
        if resp.startswith("OK NEW"):
            status = resp.split("status=")[1].split()[0]
            summary.add("ok." + status, rtt)
        elif resp.startswith("ERR NEW"):
            summary.add("err." + resp.split("code=")[1].split()[0], rtt)
        else:
            summary.add("err.protocol", rtt)

    try:
        writer.write(b"QUIT\n")
        await writer.drain()
        writer.close()
    except Exception:
        pass
    return sent


async def main_async(args) -> None:
    stop_at_ns = NS() + int(args.duration * 1e9)
    summary = Summary()
    t0 = NS()
    sent_counts = await asyncio.gather(
        *(run_conn(i, args, stop_at_ns, summary) for i in range(args.conns))
    )
    elapsed = (NS() - t0) / 1e9

    total = sum(summary.counts.values())
    sent = sum(sent_counts)
    print(f"\n--- injector summary "
          f"(sent={sent} responses={total} elapsed={elapsed:.1f}s "
          f"rate={sent / max(elapsed, 1e-9):.0f}/s target={args.rate * args.conns}/s) ---")
    for kind in sorted(summary.counts, key=lambda k: -summary.counts[k]):
        print(f"  {kind:<22} {summary.counts[kind]:>7}  "
              f"({100.0 * summary.counts[kind] / max(total, 1):.1f}%)")

    rtts = sorted(summary.rtts)
    if rtts:
        ok = [r for r in rtts]  # all responses carry an RTT
        print(f"  RTT µs: p50={percentile(ok, 50) / 1000:.0f} "
              f"p90={percentile(ok, 90) / 1000:.0f} "
              f"p99={percentile(ok, 99) / 1000:.0f} "
              f"max={ok[-1] / 1000:.0f}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5555)
    p.add_argument("--token", default="dev-alpha-token")
    p.add_argument("--rate", type=float, default=500, help="orders/sec per connection")
    p.add_argument("--duration", type=float, default=3.0, help="seconds")
    p.add_argument("--conns", type=int, default=1)
    p.add_argument("--dups", type=float, default=0.0, help="fraction of duplicate ClOrdIDs")
    p.add_argument("--fatfinger", type=float, default=0.0, help="fraction of fat-finger orders")
    p.add_argument("--noise", action="store_true",
                   help="noise-trader mode: random-side market orders (price 0)")
    args = p.parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
