// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace chatterino::tiktok {

// Runs before any TikTok page script via AddScriptToExecuteOnDocumentCreated.
// Monkey-patches window.WebSocket so every chat-socket's events are forwarded
// to the C++ host through window.chrome.webview.postMessage. Binary frames
// are base64-encoded; text frames pass through as UTF-8.
//
// Chat socket identification: TikTok's live chat currently flows over
//   wss://im-ws.tiktok.com/ws/v2?service=33554513&...
// Older endpoints used /webcast/ URLs; we match either.
//
// Also wraps window.fetch to capture the response body of
// /webcast/room/(enter|info) requests so we can surface the current
// viewer count (rendered by the page as room.user_count) without running
// our own poll or scraping the DOM.
//
// postMessage takes the payload object directly; do NOT call JSON.stringify -
// the C++ side reads via get_WebMessageAsJson which fails with E_INVALIDARG
// on string payloads.
constexpr std::wstring_view kInjectScript = LR"JS(
(function () {
    const Native = window.WebSocket;
    if (!Native || Native.__mergerinoPatched) { return; }

    const post = function (payload) {
        try {
            window.chrome.webview.postMessage(payload);
        } catch (e) { /* host gone - ignore */ }
    };

    const b64 = function (buf) {
        const bytes = new Uint8Array(buf);
        let bin = '';
        for (let i = 0; i < bytes.length; i++) {
            bin += String.fromCharCode(bytes[i]);
        }
        return window.btoa(bin);
    };

    const isChatSocket = function (u) {
        return /im-ws\.tiktok\.com\/ws|webcast/i.test(u);
    };

    const Patched = function (url, protocols) {
        const ws = new Native(url, protocols);
        const u = String(url);
        if (!isChatSocket(u)) { return ws; }

        post({ kind: 'ws-open', url: u });

        ws.addEventListener('close', function (e) {
            post({ kind: 'ws-close', url: u, code: e.code, reason: e.reason });
        });
        ws.addEventListener('error', function () {
            post({ kind: 'ws-error', url: u });
        });
        ws.addEventListener('message', function (e) {
            if (e.data instanceof ArrayBuffer) {
                post({ kind: 'ws-binary', url: u, size: e.data.byteLength, base64: b64(e.data) });
            } else if (e.data instanceof Blob) {
                e.data.arrayBuffer().then(function (buf) {
                    post({ kind: 'ws-binary', url: u, size: buf.byteLength, base64: b64(buf) });
                });
            } else {
                post({ kind: 'ws-text', url: u, text: String(e.data) });
            }
        });

        return ws;
    };

    Patched.prototype = Native.prototype;
    Patched.CONNECTING = Native.CONNECTING;
    Patched.OPEN = Native.OPEN;
    Patched.CLOSING = Native.CLOSING;
    Patched.CLOSED = Native.CLOSED;
    Patched.__mergerinoPatched = true;
    Object.setPrototypeOf(Patched, Native);

    window.WebSocket = Patched;

    const isRoomInfoRequest = function (u) {
        return /webcast\/room\/(enter|info|check_alive)/i.test(String(u));
    };

    const NativeFetch = window.fetch;
    if (typeof NativeFetch === 'function' && !NativeFetch.__mergerinoPatched) {
        const PatchedFetch = function (input, init) {
            let u = '';
            try {
                u = typeof input === 'string' ? input : (input && input.url) || '';
            } catch (e) { /* ignore */ }
            const promise = NativeFetch.apply(this, arguments);
            if (isRoomInfoRequest(u)) {
                promise.then(function (resp) {
                    return resp.clone().json();
                }).then(function (json) {
                    post({ kind: 'room-info', url: String(u), data: json });
                }).catch(function () { /* ignore */ });
            }
            return promise;
        };
        PatchedFetch.__mergerinoPatched = true;
        window.fetch = PatchedFetch;
    }
})();
)JS";

}  // namespace chatterino::tiktok
