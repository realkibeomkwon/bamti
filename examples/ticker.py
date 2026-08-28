#!/usr/bin/env python3
"""Push a ticker Status Item (icon + value + toggle) over \\\\.\\pipe\\bamti-status."""

from __future__ import annotations

import ctypes
import json
import msvcrt
import threading
import time

PIPE_NAME = r"\\.\pipe\bamti-status"
ITEM_ID = "example.ticker.btc"


def connect():
    deadline = time.monotonic() + 5
    err = None
    while time.monotonic() < deadline:
        try:
            return open(PIPE_NAME, "r+b", buffering=0)
        except OSError as exc:
            err = exc
            time.sleep(0.15)
    raise SystemExit(f"could not open {PIPE_NAME}: {err}")


def send(pipe, payload: dict) -> None:
    pipe.write(json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")
    pipe.flush()


def peek_bytes(pipe) -> int:
    handle = msvcrt.get_osfhandle(pipe.fileno())
    avail = ctypes.c_ulong(0)
    if ctypes.windll.kernel32.PeekNamedPipe(ctypes.c_void_p(handle), None, 0, None, ctypes.byref(avail), None) == 0:
        raise OSError(ctypes.windll.kernel32.GetLastError())
    return int(avail.value)


def item(price: int, alerts: bool) -> dict:
    return {
        "v": 2,
        "op": "upsert",
        "id": ITEM_ID,
        "segment": {"icon": {"kind": "glyph", "glyph": "₿"}, "text": f"{price:,}", "priority": 8},
        "panel": {
            "title": "BTC",
            "rows": [
                {"type": "kv", "label": "Last", "value": f"{price:,} USD"},
                {"type": "toggle", "row_id": "alerts", "label": "Alerts", "on": alerts},
            ],
        },
    }


def main() -> int:
    pipe = connect()
    price = 97000
    alerts = True
    send(pipe, item(price, alerts))
    stop = threading.Event()
    lock = threading.Lock()

    def ping_loop() -> None:
        while not stop.wait(5.0):
            with lock:
                try:
                    send(pipe, {"v": 2, "op": "ping"})
                except OSError:
                    stop.set()
                    return

    def tick() -> None:
        nonlocal price
        while not stop.wait(30):
            price += 17
            with lock:
                send(pipe, {"v": 2, "op": "patch", "id": ITEM_ID, "segment": {"text": f"{price:,}"},
                            "panel": item(price, alerts)["panel"]})

    threading.Thread(target=ping_loop, daemon=True).start()
    threading.Thread(target=tick, daemon=True).start()
    buf = b""
    try:
        while not stop.is_set():
            try:
                n = peek_bytes(pipe)
            except OSError:
                break
            if n == 0:
                time.sleep(0.1)
                continue
            try:
                chunk = pipe.read(n)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                if not raw:
                    continue
                try:
                    ev = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if ev.get("event") == "toggle" and ev.get("row_id") == "alerts":
                    alerts = bool(ev.get("on"))
                    with lock:
                        send(pipe, {"v": 2, "op": "patch", "id": ITEM_ID, "panel": item(price, alerts)["panel"]})
    except (KeyboardInterrupt, OSError):
        pass
    stop.set()
    with lock:
        try:
            send(pipe, {"v": 2, "op": "remove", "id": ITEM_ID})
        except OSError:
            pass
    pipe.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
