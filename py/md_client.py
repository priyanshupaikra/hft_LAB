#!/usr/bin/env python3
"""hft-lab market-data client: consumes the top-of-book stream and
demonstrates gap detection + snapshot recovery.

The stream carries per-symbol sequence numbers. If we see seq jump past
last+1, updates were dropped (slow consumer) or we joined mid-stream — the
recovery is the standard snapshot+delta dance: SNAP the gateway, take its
sequence number, keep only newer deltas.

  python3 py/md_client.py --md-port 7600 --port 5555 [--quiet]
"""
import argparse
import asyncio


def parse_frame(line: str):
    """'MD sym=1 seq=5 bid=10000 ask=10050' -> dict"""
    fields = dict(kv.split("=", 1) for kv in line.split()[1:])
    return {k: int(v) for k, v in fields.items()}


async def snap_via_gateway(host, gateway_port, symbol: int):
    """One-shot gateway session: AUTH admin, SNAP, return (seq, bid, ask)."""
    reader, writer = await asyncio.open_connection(host, gateway_port)

    async def rpc(line: str) -> str:
        writer.write((line + "\n").encode())
        await writer.drain()
        return (await reader.readline()).decode().strip()

    await rpc("AUTH dev-admin-token")
    resp = await rpc(f"SNAP {symbol}")
    writer.write(b"QUIT\n")
    await writer.drain()
    if not resp.startswith("OK SNAP"):
        raise SystemExit(f"snap failed: {resp}")
    f = dict(kv.split("=", 1) for kv in resp.split()[2:])
    return int(f["seq"]), int(f["bid"]), int(f["ask"])


async def main_async(args) -> None:
    reader, _writer = await asyncio.open_connection(args.host, args.md_port)
    print(f"md_client: subscribed to {args.host}:{args.md_port}")
    last_seq = {}  # symbol -> last seen sequence number
    gaps = 0

    while True:
        raw = await reader.readline()
        if not raw:
            print("md_client: stream closed by publisher")
            break
        line = raw.decode().strip()
        if not line.startswith("MD "):
            continue
        f = parse_frame(line)
        sym, seq = f["sym"], f["seq"]
        known = last_seq.get(sym)
        if known is not None and seq > known + 1:
            gaps += 1
            snap_seq, bid, ask = await snap_via_gateway(args.host, args.port, sym)
            print(f"  [gap] sym={sym} expected seq={known + 1} got {seq} "
                  f"-> SNAP resync seq={snap_seq} bid={bid} ask={ask}")
            if seq <= snap_seq:
                continue  # stale delta after snapshot: skip
        last_seq[sym] = seq
        if not args.quiet:
            print(f"  sym={sym} seq={seq} bid={f['bid']} ask={f['ask']}")

    print(f"md_client: {gaps} gap(s) recovered via snapshot")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--md-port", type=int, default=7600)
    p.add_argument("--port", type=int, default=5555, help="gateway port for SNAPs")
    p.add_argument("--quiet", action="store_true", help="only print gaps")
    args = p.parse_args()
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("\nmd_client: bye")


if __name__ == "__main__":
    main()
