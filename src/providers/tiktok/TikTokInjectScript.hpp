// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace chatterino::tiktok {

// Runs before any TikTok page script via AddScriptToExecuteOnDocumentCreated.
// Monkey-patches window.WebSocket so every /webcast/ socket's events are
// forwarded to the C++ host through window.chrome.webview.postMessage. Binary
// frames are base64-encoded; text frames pass through as UTF-8.
constexpr std::wstring_view kInjectScript = LR"JS(
(function () {
    const Native = window.WebSocket;
    if (!Native || Native.__mergerinoPatched) { return; }

    const post = function (payload) {
        try {
            window.chrome.webview.postMessage(JSON.stringify(payload));
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

    const Patched = function (url, protocols) {
        const ws = new Native(url, protocols);
        const u = String(url);
        if (!/webcast/i.test(u)) { return ws; }

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
})();
)JS";

}  // namespace chatterino::tiktok
