#!/usr/bin/env python3
"""hft-lab toy market maker.

Quotes a bid and an ask around a drifting "fundamental" price (a random
walk standing in for real signal), skews its quotes against inventory,
cancels-and-requotes on a timer, and polls EVENTS for maker fills — the
polling side of the push-vs-poll trade-off, deliberately.

PnL accounting is mark-to-fundamental: cash + inventory * ref. With
balanced noise-trader flow the spread earns money; skew bleeds inventory
risk off gradually. Adverse selection, queue position, and fees are all
consciously ignored — this is a systems demo, not an alpha.

  python3 py/mm/mm.py --port 5555 --duration 30
"""
import argparse
import asyncio
import random
import time


class GatewaySession:
    def __init__(self, host: str, port: int, token: str):
        self.host, self.port, self.token = host, port, token

    async def connect(self):
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)

    async def rpc(self, line: str) -> str:
        self.writer.write((line + "\n").encode())
        await self.writer.drain()
        return (await self.reader.readline()).decode().strip()

    async def events(self) -> list[str]:
        self.writer.write(b"EVENTS\n")
        await self.writer.drain()
        out = []
        while True:
            line = (await self.reader.readline()).decode().strip()
            if line == "EVENTS_DONE":
                return out
            out.append(line)


async def run(args) -> None:
    gw = GatewaySession(args.host, args.port, args.token)
    await gw.connect()
    auth = await gw.rpc(f"AUTH {args.token}")
    if not auth.startswith("OK"):
        raise SystemExit(f"auth failed: {auth}")
    print(f"mm: {auth}")

    rng = random.Random(42)
    ref = 10_000            # fundamental price, cents
    half_spread = 20        # cents each side
    skew_cents_per_unit = 4  # how hard we lean against inventory
    quote_qty = 5

    cl_next = 1
    cash = 0                # cents, positive = collected
    inventory = 0           # shares, positive = long
    fills = 0
    bid_cl = ask_cl = None

    def next_cl() -> int:
        nonlocal cl_next
        cl_next += 1
        return cl_next

    def apply_fill(side: str, px: int, qty: int):
        nonlocal cash, inventory, fills
        fills += 1
        if side == "B":     # our bid got lifted: we bought
            cash -= px * qty
            inventory += qty
        else:               # our offer got hit: we sold
            cash += px * qty
            inventory -= qty

    start = time.time()
    next_requote = 0.0
    next_status = 2.0

    while True:
        now = time.monotonic()
        elapsed = time.time() - start
        if elapsed >= args.duration:
            break

        # 1) Drain maker fills first so cancel/requote sees fresh state.
        for line in await gw.events():
            if line.startswith("FILL"):
                f = dict(kv.split("=", 1) for kv in line.split()[1:])
                side = "B" if int(f["cl"]) == bid_cl else "S"
                apply_fill(side, int(f["px"]), int(f["qty"]))

        # 2) Requote on the timer: cancel both, then post fresh quotes.
        if now >= next_requote:
            next_requote = now + args.requote_ms / 1000.0
            for cl in (bid_cl, ask_cl):
                if cl is not None:
                    await gw.rpc(f"CXL {cl}")  # UNKNOWN => it already filled
            ref = max(100, ref + rng.gauss(0, 3))  # the "fundamental" walks
            offset = half_spread + skew_cents_per_unit * inventory
            bid_cl = next_cl()
            ask_cl = next_cl()
            await gw.rpc(f"NEW {bid_cl} B {quote_qty} {max(1, int(ref - offset))}")
            await gw.rpc(f"NEW {ask_cl} S {quote_qty} {int(ref + offset) + 1}")

        # 3) Status line.
        if now >= next_status:
            next_status = now + 2.0
            pnl = cash + inventory * ref
            print(f"mm: t={elapsed:5.1f}s ref={ref:8.1f} inv={inventory:+4d} "
                  f"cash={cash / 100:10.2f}$ pnl={pnl / 100:8.2f}$ fills={fills}")

    # Final summary (cancel leftovers so the book is clean).
    for cl in (bid_cl, ask_cl):
        if cl is not None:
            await gw.rpc(f"CXL {cl}")
    pnl = cash + inventory * ref
    print(f"\nmm: done. fills={fills} final_inventory={inventory} "
          f"mark_to_ref_pnl={pnl / 100:.2f}$ "
          f"(spread earned vs inventory risk — see module docstring)")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5555)
    p.add_argument("--token", default="dev-alpha-token")
    p.add_argument("--duration", type=float, default=30.0)
    p.add_argument("--requote-ms", type=float, default=200.0)
    args = p.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
