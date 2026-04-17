# SPDX-FileCopyrightText: 2026 Mergerino
# SPDX-License-Identifier: MIT
#
# TikTok LIVE ingestion spike: probes what signing/auth TikTok requires for a
# read-only guest before we commit to a C++ architecture.
#
# Usage:
#   python spike.py <user-or-url>              scrape + detect live state
#   python spike.py <user-or-url> --ws         open WebSocket, dump frames
#
# --ws needs `pip install websocket-client`. Everything else is stdlib only.
# Output (html/json/frames) goes into ./out/.

import argparse
import http.cookiejar
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)


def normalize(ref: str) -> str:
    ref = ref.strip()
    m = re.match(r"https?://(?:www\.|m\.)?tiktok\.com/@([^/?#]+)", ref)
    if m:
        return m.group(1)
    return ref.lstrip("@")


def make_opener():
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    opener.addheaders = [
        ("User-Agent", USER_AGENT),
        ("Accept-Language", "en-US,en;q=0.9"),
    ]
    return opener, jar


def fetch(opener, url: str, accept: str) -> tuple[int, bytes, dict]:
    req = urllib.request.Request(url, headers={"Accept": accept})
    try:
        with opener.open(req, timeout=20) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers or {})


def parse_hydration(html: str) -> dict:
    blobs = {}
    for script_id in ("__UNIVERSAL_DATA_FOR_REHYDRATION__", "SIGI_STATE"):
        m = re.search(
            r'<script id="' + script_id + r'"[^>]*>(.*?)</script>',
            html,
            re.S,
        )
        if m:
            try:
                blobs[script_id] = json.loads(m.group(1))
            except json.JSONDecodeError:
                pass
    return blobs


def find_room(blobs: dict) -> tuple[str | None, str | None, int]:
    sigi = blobs.get("SIGI_STATE", {})
    user = (sigi.get("LiveRoom", {})
                .get("liveRoomUserInfo", {})
                .get("user", {}))
    room_id = user.get("roomId")
    status = user.get("status", 0)
    if room_id and room_id != "0":
        return "SIGI_STATE.LiveRoom.liveRoomUserInfo.user.roomId", room_id, status
    uni = blobs.get("__UNIVERSAL_DATA_FOR_REHYDRATION__", {})
    scope = uni.get("__DEFAULT_SCOPE__", {})
    flat = json.dumps(scope)
    for pat in (r'"roomId"\s*:\s*"?(\d{15,25})"?',
                r'"room_id"\s*:\s*"?(\d{15,25})"?'):
        m = re.search(pat, flat)
        if m:
            return f"UNIVERSAL scope match {pat}", m.group(1), 2
    return None, None, 0


def webcast_fetch_url(room_id: str, cursor: str = "0") -> str:
    params = {
        "aid": "1988",
        "app_language": "en-US",
        "app_name": "tiktok_web",
        "browser_language": "en-US",
        "browser_name": "Mozilla",
        "browser_online": "true",
        "browser_platform": "Win32",
        "browser_version": "5.0 (Windows)",
        "cookie_enabled": "true",
        "cursor": cursor,
        "device_platform": "web",
        "identity": "audience",
        "internal_ext": "",
        "last_rtt": "0",
        "live_id": "12",
        "resp_content_type": "protobuf",
        "room_id": room_id,
        "screen_height": "1080",
        "screen_width": "1920",
        "tz_name": "Europe/Helsinki",
        "version_code": "270000",
    }
    return "https://webcast.tiktok.com/webcast/im/fetch/?" + urllib.parse.urlencode(params)


def dump_webcast(body: bytes, out: Path) -> dict | None:
    (out / "webcast-fetch.bin").write_bytes(body)
    try:
        j = json.loads(body)
        (out / "webcast-fetch.json").write_text(
            json.dumps(j, indent=2), encoding="utf-8"
        )
        return j
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def extract_ws_url(j: dict) -> str | None:
    if not isinstance(j, dict):
        return None
    data = j.get("data", j)
    for key in ("wsUrl", "ws_url", "push_server", "pushServer"):
        v = data.get(key) if isinstance(data, dict) else None
        if isinstance(v, str) and v.startswith(("ws://", "wss://")):
            return v
    return None


def run_ws(ws_url: str, max_frames: int, out: Path) -> None:
    try:
        import websocket
    except ImportError:
        print("[ws] websocket-client not installed; skipping")
        print("     install with: pip install websocket-client")
        return

    frames: list[bytes] = []

    def on_open(ws):
        print(f"[ws] open: {ws_url[:100]}...")

    def on_message(ws, msg):
        data = msg.encode() if isinstance(msg, str) else msg
        frames.append(data)
        preview = data[:24].hex()
        print(f"[ws] frame #{len(frames)}: {len(data)} bytes, head={preview}")
        if len(frames) >= max_frames:
            ws.close()

    def on_error(ws, err):
        print(f"[ws] error: {err}")

    def on_close(ws, code, reason):
        print(f"[ws] close: code={code} reason={reason}")

    ws = websocket.WebSocketApp(
        ws_url,
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
        header=[f"User-Agent: {USER_AGENT}"],
    )
    ws.run_forever(ping_interval=10, ping_timeout=5)

    for i, f in enumerate(frames):
        (out / f"frame-{i:03d}.bin").write_bytes(f)
    print(f"[ws] captured {len(frames)} frame(s) to {out}/frame-NNN.bin")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("user", help="TikTok username, @handle, or /@user/live URL")
    ap.add_argument("--ws", action="store_true",
                    help="Open the returned WebSocket and dump frames")
    ap.add_argument("--frames", type=int, default=5,
                    help="Max frames to capture (default 5)")
    ap.add_argument("--out", default="out",
                    help="Output directory (default ./out)")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(exist_ok=True)

    username = normalize(args.user)
    print(f"[1/6] resolving: @{username}")

    opener, jar = make_opener()

    live_url = f"https://www.tiktok.com/@{username}/live"
    status, body, headers = fetch(opener, live_url, "text/html")
    print(f"[2/6] GET {live_url}")
    print(f"      -> HTTP {status}, {len(body)} bytes, ct={headers.get('Content-Type', '?')}")

    if status != 200:
        print("[!] page fetch failed - TikTok may be gating anonymous UAs")
        (out / "live.html").write_bytes(body)
        return 1

    html = body.decode("utf-8", "replace")
    (out / "live.html").write_text(html, encoding="utf-8")

    cookies = {c.name: c.value[:32] + ("..." if len(c.value) > 32 else "") for c in jar}
    print(f"[3/6] cookies set: {sorted(cookies.keys())}")

    blobs = parse_hydration(html)
    if not blobs:
        print("[!] no hydration blobs found - structure changed?")
        return 1
    for name, blob in blobs.items():
        (out / f"{name.lower()}.json").write_text(
            json.dumps(blob, indent=2), encoding="utf-8"
        )
    print(f"[4/6] hydration blobs: {list(blobs.keys())}")

    where, room_id, status = find_room(blobs)
    if not room_id:
        print("[5/6] offline - no roomId in hydration")
        return 0
    print(f"[5/6] LIVE: roomId={room_id} status={status} (from {where})")

    fetch_url = webcast_fetch_url(room_id)
    status, body, headers = fetch(opener, fetch_url, "application/json, */*")
    print(f"[6/6] GET /webcast/im/fetch/")
    print(f"      -> HTTP {status}, {len(body)} bytes, ct={headers.get('Content-Type', '?')}")

    j = dump_webcast(body, out)
    if j is None:
        print("      (binary or empty body - saved to webcast-fetch.bin)")
    else:
        top = list(j.keys()) if isinstance(j, dict) else type(j).__name__
        print(f"      JSON top keys: {top}")
        status_code = j.get("status_code") if isinstance(j, dict) else None
        if status_code and status_code != 0:
            print(f"      ERROR status_code={status_code} msg={j.get('status_msg')}")

    if args.ws:
        ws_url = extract_ws_url(j) if j else None
        if not ws_url:
            print("[ws] no WebSocket URL in response; cannot continue")
            return 0
        run_ws(ws_url, args.frames, out)

    return 0


if __name__ == "__main__":
    sys.exit(main())
