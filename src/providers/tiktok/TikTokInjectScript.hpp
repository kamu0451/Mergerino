// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace chatterino::tiktok {

// Runs before any TikTok page script via AddScriptToExecuteOnDocumentCreated.
// Monkey-patches window.WebSocket so every /webcast/ socket's events are
// forwarded to the C++ host through window.chrome.webview.postMessage. Binary
// frames are base64-encoded; text frames pass through as UTF-8.
//
// Diagnostics: also reports every WebSocket URL the page opens (kind:
// 'ws-probe'), every /webcast|im\/fetch/ HTTP request (kind: 'http-probe'),
// and the final navigated URL on DOMContentLoaded (kind: 'nav-end'). These
// surface as system messages so we can diagnose why a live page isn't
// producing a chat WebSocket.
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

    const isChatSocket = function (u) {
        return /webcast/i.test(u);
    };

    const Patched = function (url, protocols) {
        const ws = new Native(url, protocols);
        const u = String(url);

        post({ kind: 'ws-probe', url: u });

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

    const isInterestingHttp = function (u) {
        return /webcast|im\/fetch|im\/push/i.test(String(u));
    };

    const NativeFetch = window.fetch;
    if (typeof NativeFetch === 'function' && !NativeFetch.__mergerinoPatched) {
        const PatchedFetch = function (input, init) {
            let u = '';
            try {
                u = typeof input === 'string' ? input : (input && input.url) || '';
            } catch (e) { /* ignore */ }
            if (isInterestingHttp(u)) {
                post({ kind: 'http-probe', via: 'fetch', url: String(u) });
            }
            return NativeFetch.apply(this, arguments);
        };
        PatchedFetch.__mergerinoPatched = true;
        window.fetch = PatchedFetch;
    }

    const XHRopen = window.XMLHttpRequest && window.XMLHttpRequest.prototype
        && window.XMLHttpRequest.prototype.open;
    if (XHRopen && !XHRopen.__mergerinoPatched) {
        window.XMLHttpRequest.prototype.open = function (method, url) {
            try {
                if (isInterestingHttp(url)) {
                    post({ kind: 'http-probe', via: 'xhr', url: String(url) });
                }
            } catch (e) { /* ignore */ }
            return XHRopen.apply(this, arguments);
        };
        window.XMLHttpRequest.prototype.open.__mergerinoPatched = true;
    }

    const reportNav = function () {
        try {
            post({ kind: 'nav-end', url: String(window.location.href),
                   title: String(document.title || '') });
        } catch (e) { /* ignore */ }
    };
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', reportNav, { once: true });
    } else {
        reportNav();
    }
})();
)JS";

}  // namespace chatterino::tiktok
