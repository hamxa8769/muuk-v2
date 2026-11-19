#!/usr/bin/env python3
"""SYNTHETIC CME book and option chain, for demo tests of the regime-range feed.

Made-up numbers, not market data. It proves the plumbing only: replace it with
a feeder that reads a real CME source and sends the same lines (the format is
in docs/regime-range.md).

    python tools/feeds/sim_feed.py --mid 4340 --port 5310
"""
import argparse
import json
import random
import socket
import time


def book(mid):
    def levels(sign):
        return [[round(mid + sign * (0.1 + i), 2), random.randint(5, 60)] for i in range(10)]

    bids, asks = levels(-1), levels(1)
    bids[random.randint(0, 9)][1] = 400
    asks[random.randint(0, 9)][1] = 400
    return {"t": "book", "bids": bids, "asks": asks}


def options(mid):
    base = round(mid / 5) * 5
    return {"t": "options", "strikes": [[base + i * 5, random.randint(100, 900), random.randint(100, 900)]
                                         for i in range(-8, 9)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mid", type=float, required=True, help="the future's price to centre on")
    parser.add_argument("--port", type=int, default=5310)
    args = parser.parse_args()

    mid = args.mid
    with socket.create_connection(("127.0.0.1", args.port)) as s:
        sent = 0
        while True:
            mid += random.uniform(-0.2, 0.2)
            lines = [book(mid)]
            if sent % 60 == 0:
                lines.append(options(mid))
            s.sendall("".join(json.dumps(line) + "\n" for line in lines).encode())
            sent += 1
            time.sleep(1)


if __name__ == "__main__":
    main()
