#!/usr/bin/env python3
"""Push Status Items to bamti over \\\\.\\pipe\\bamti-status (NDJSON)."""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time

PIPE_NAME = r"\\.\pipe\bamti-status"


def connect(timeout_ms: int = 5000):
    deadline = time.monotonic() + timeout_ms / 1000.0
    last_error = None
    while time.monotonic() < deadline:
        try:
            return open(PIPE_NAME, "r+b", buffering=0)
        except OSError as exc:
            last_error = exc
            time.sleep(0.15)
    raise SystemExit(f"could not open {PIPE_NAME}: {last_error}")


def send(pipe, payload: dict) -> None:
    line = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    pipe.write(line + b"\n")
    pipe.flush()


def usage_example_v2(item_id: str) -> dict:
    return {
        "v": 2,
        "op": "upsert",
        "id": item_id,
        "segment": {
            "icon": {"kind": "glyph", "glyph": "◐"},
            "text": "10%",
            "tooltip": "Codex · Session 6% · Weekly 10%",
            "state": "normal",
            "accent": "#FF7A45",
            "priority": 10,
        },
        "panel": {
            "title": "Codex",
            "subtitle": "Prolite",
            "updated": "Updated just now",
            "rows": [
                {
                    "type": "gauge",
                    "label": "Session (5h) — Codex",
                    "value": 0.06,
                    "value_text": "6%",
                    "detail": "Resets later",
                },
                {
                    "type": "gauge",
                    "label": "Weekly — Codex",
                    "value": 0.10,
                    "value_text": "10%",
                    "detail": "Resets in 6d",
                    "note": "Ahead of pace",
                },
                {"type": "separator"},
                {"type": "kv", "label": "Plan", "value": "Pro"},
                {"type": "text", "text": "Values are illustrative.", "style": "note"},
                {"type": "toggle", "row_id": "auto_refresh", "label": "Auto refresh", "on": True},
                {"type": "button", "row_id": "settings", "label": "Settings..."},
                {"type": "button", "row_id": "quit", "label": "Quit", "style": "danger"},
            ],
        },
    }


def usage_example(item_id: str) -> dict:
    return {
        "v": 1,
        "op": "upsert",
        "id": item_id,
        "text": "10%",
        "icon": "⚡",
        "tooltip": "Codex usage",
        "accent": 0x2BD9C7,
        "priority": 10,
        "panel": {
            "title": "Codex",
            "subtitle": "Prolite",
            "updated": "Updated just now",
            "gauges": [
                {
                    "label": "Session (5h)",
                    "value": 0.10,
                    "detail": "Resets later",
                    "note": "Ahead of pace",
                },
                {
                    "label": "Weekly",
                    "value": 0.42,
                    "detail": "42%",
                    "note": "",
                },
            ],
            "actions": ["Settings...", "Quit"],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="bamti Status Item example client")
    parser.add_argument("--id", default="example.codex.usage")
    parser.add_argument("--text", default="")
    parser.add_argument("--remove", action="store_true")
    parser.add_argument("--once", action="store_true", help="upsert once and exit")
    parser.add_argument("--interval", type=float, default=10.0, help="ping interval seconds")
    parser.add_argument("--v2", action="store_true", help="send protocol v2 example")
    args = parser.parse_args()
    ver = 2 if args.v2 else 1

    pipe = connect()
    try:
        if args.remove:
            send(pipe, {"v": ver, "op": "remove", "id": args.id})
            return 0

        item = usage_example_v2(args.id) if args.v2 else usage_example(args.id)
        if args.text:
            if args.v2:
                item.setdefault("segment", {})["text"] = args.text
            else:
                item["text"] = args.text
        send(pipe, item)
        if args.once:
            return 0

        print(f"pushed {args.id}; reading click events (Ctrl+C to quit)", file=sys.stderr)
        write_lock = threading.Lock()
        stop = threading.Event()

        def ping_loop() -> None:
            while not stop.wait(args.interval):
                with write_lock:
                    try:
                        send(pipe, {"v": ver, "op": "ping"})
                    except OSError:
                        stop.set()
                        return

        pinger = threading.Thread(target=ping_loop, daemon=True)
        pinger.start()
        buf = b""
        try:
            while not stop.is_set():
                try:
                    chunk = pipe.read(1)
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
                        event = json.loads(raw.decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        continue
                    print(event, flush=True)
        finally:
            stop.set()
            with write_lock:
                try:
                    send(pipe, {"v": ver, "op": "remove", "id": args.id})
                except OSError:
                    pass
        return 0
    except KeyboardInterrupt:
        return 0
    finally:
        pipe.close()


if __name__ == "__main__":
    raise SystemExit(main())
