#!/usr/bin/env python3
"""
Codex PermissionRequest hook → Doudou Bridge.

Codex Desktop invokes this script on every tool-permission request,
piping the hook JSON on stdin. We forward it to the local Doudou Bridge
HTTP entrypoint, wait for the device to reply, and echo the resulting
hook-decision JSON on stdout.

Install:
    1. Bridge running on http://127.0.0.1:8788/ (default port).
    2. Make this script executable: chmod +x scripts/permission_request_hook.py
    3. Add to ~/.codex/hooks.json:

       {
         "hooks": {
           "PermissionRequest": [
             {
               "matcher": "*",
               "hooks": [
                 {
                   "type": "command",
                   "command": "/abs/path/to/scripts/permission_request_hook.py",
                   "timeout": 120,
                   "statusMessage": "Waiting for Doudou approval"
                 }
               ]
             }
           ]
         }
       }

Env overrides:
    DOUDOU_BRIDGE_URL       full URL (default: http://127.0.0.1:8788/approval/request)
    DOUDOU_APPROVAL_TIMEOUT seconds to wait for Bridge reply (default: 120)

Graceful-degrade behaviour:
    Any error here (Bridge unreachable, timeout, malformed reply) prints
    an empty JSON object `{}` on stdout. Codex treats that as "no
    decision" and falls back to its own approval prompt — never lock
    out the user because Doudou is down.
"""
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request


BRIDGE_URL = os.environ.get("DOUDOU_BRIDGE_URL", "http://127.0.0.1:8788/approval/request")
TIMEOUT_S  = int(os.environ.get("DOUDOU_APPROVAL_TIMEOUT", "120"))


def passthrough() -> None:
    """Emit no-decision JSON so Codex falls back to its own UI."""
    print("{}")
    sys.exit(0)


def main() -> None:
    raw = sys.stdin.read()
    if not raw.strip():
        passthrough()
        return

    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        passthrough()
        return

    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BRIDGE_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
            sys.stdout.write(resp.read().decode("utf-8", errors="replace"))
            sys.stdout.flush()
    except (urllib.error.URLError, TimeoutError, OSError):
        # Bridge unreachable / hung. Fall back silently.
        passthrough()
        return


if __name__ == "__main__":
    main()
