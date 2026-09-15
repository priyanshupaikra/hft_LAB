#!/usr/bin/env python3
"""hft-lab monitor: polls the gateway STATS endpoint like an ops dashboard.

Concept demo: this is the *polling* side of webhooks-vs-polling. STATS is
pulled on an interval; the push alternative (server-streamed events) is the
ROADMAP Phase 5 market-data feed. Also exposes one-shot --kill / --resume
(the trader's big red button).

  python3 py/monitor/monitor.py --rounds 5
  python3 py/monitor/monitor.py --kill
"""
import argparse
import asyncio

COLUMNS = [
    ("accepted", "accepted"),
    ("fills", "fills"),
    ("resting", "resting"),
    ("rate_rej", "rate_rej"),
    ("dup_rej", "dup_rej"),
    ("risk_rej", "risk_rej"),
    ("kill_rej", "kill_rej"),
    ("kill", "kill"),
    ("core_lat_p50_ns", "p50(ns)"),
    ("core_lat_p99_ns", "p99(ns)"),
]


async def rpc(reader, writer, line: str) -> str:
    writer.write((line + "\n").encode())
    await writer.drain()
    return (await reader.readline()).decode().strip()


async def main_async(args) -> None:
    reader, writer = await asyncio.open_connection(args.host, args.port)
    resp = await rpc(reader, writer, f"AUTH {args.token}")
    if not resp.startswith("OK"):
        raise SystemExit(f"auth failed: {resp}")

    if args.kill or args.resume:
        cmd = "KILL" if args.kill else "RESUME"
        print(await rpc(reader, writer, cmd))
        writer.write(b"QUIT\n")
        await writer.drain()
        return

    header = "  ".join(f"{label:>12}" for _, label in COLUMNS)
    print(header)
    for round_no in range(args.rounds):
        resp = await rpc(reader, writer, "STATS")
        fields = dict(kv.split("=", 1) for kv in resp.split()[2:])
        row = "  ".join(f"{fields.get(key, '-')[:12]:>12}" for key, _ in COLUMNS)
        print(row, flush=True)
        if round_no + 1 < args.rounds:
            await asyncio.sleep(args.interval)

    writer.write(b"QUIT\n")
    await writer.drain()


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5555)
    p.add_argument("--token", default="dev-admin-token")
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--kill", action="store_true", help="trip the kill switch and exit")
    p.add_argument("--resume", action="store_true", help="reset the kill switch and exit")
    args = p.parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
