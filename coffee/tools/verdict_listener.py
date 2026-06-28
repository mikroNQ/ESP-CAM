#!/usr/bin/env python3
"""Tiny TCP sink for CoffeeVerifier NDJSON events — pretty-prints verdicts.

The firmware's TCP reporter connects out to this host:port (set it with
  curl "http://coffeecam.local/cupcfg?host=<this-pc-ip>&port=9000"
) and streams one JSON object per line: a "hello" on connect, periodic "status"
heartbeats, and a "verdict" per /verify. This script accepts one connection at a
time and prints each line, highlighting NOT_DISPENSED (the money problem).

No dependencies (stdlib only):
  python3 coffee/tools/verdict_listener.py            # listen on 0.0.0.0:9000
  python3 coffee/tools/verdict_listener.py --port 9100
"""
import argparse
import json
import socket
from datetime import datetime

RESET = "\033[0m"
RED = "\033[31;1m"
GREEN = "\033[32;1m"
YELLOW = "\033[33m"
DIM = "\033[2m"


def fmt(line: str) -> str:
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return f"{DIM}{line}{RESET}"
    typ = obj.get("type")
    if typ == "hello":
        return f"{GREEN}● connected{RESET} dev={obj.get('dev')} ip={obj.get('ip')}"
    if typ == "status":
        return (f"{DIM}· status rssi={obj.get('rssi')}dBm "
                f"uptime={obj.get('uptime_s')}s heap={obj.get('heap')}{RESET}")
    if typ == "dropped":
        return f"{YELLOW}! dropped {obj.get('count')} verdict(s){RESET}"
    if typ == "verdict":
        result = obj.get("result")
        colour = GREEN if result == "ok" else RED
        return (f"{colour}VERDICT #{obj.get('seq')} {result.upper()}{RESET} "
                f"expected={obj.get('expected')} dispensed={obj.get('dispensed')} "
                f"milk={obj.get('milk')} fill_ms={obj.get('fill_ms')} "
                f"delta={obj.get('delta')} y:{obj.get('baseline_y')}→{obj.get('final_y')}")
    return line


def serve(host: str, port: int) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"listening on {host}:{port} — point the camera here with "
          f"/cupcfg?host=<ip>&port={port}")
    while True:
        conn, addr = srv.accept()
        print(f"{GREEN}[{datetime.now():%H:%M:%S}] camera connected from "
              f"{addr[0]}:{addr[1]}{RESET}")
        buf = b""
        try:
            with conn:
                while True:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").strip()
                        if line:
                            print(f"[{datetime.now():%H:%M:%S}] {fmt(line)}")
        except ConnectionResetError:
            pass
        print(f"{YELLOW}[{datetime.now():%H:%M:%S}] camera disconnected{RESET}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9000)
    args = ap.parse_args()
    try:
        serve(args.host, args.port)
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
