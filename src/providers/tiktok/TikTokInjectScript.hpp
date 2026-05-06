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
//
// MSVC caps a single string literal at ~16380 bytes. Wide raw-string
// literals double the byte count per ASCII char, so the script is split
// into adjacent literals - the compiler concatenates them.
constexpr std::wstring_view kInjectScript = LR"JS(
(function () {
    const Native = window.WebSocket;
    if (!Native || Native.__mergerinoPatched) { return; }

    // Spoof Page Visibility API so TikTok's webcast client believes the
    // tab is foreground and starts the chat-event subscription. The host
    // window is hidden 1x1 for perf; the actual chat WebSocket frames
    // we capture are heartbeat-only otherwise (verified via frame dumps:
    // 1680-byte periodic poll, no chat methods). OBS shipped the same
    // override for its CEF browser source - obs-browser PR #168.
    // Spoof IntersectionObserver to immediately report each observed
    // target as visible. Many React components - including TikTok's
    // chat list, suspected based on dom-snapshot showing the container
    // empty for >30s while viewport reports 1281x1392 - lazy-load on
    // first intersection. Without this, hidden 1x1 hosts stay forever
    // pre-intersection and chat never subscribes.
    try {
        const NativeIO = window.IntersectionObserver;
        if (NativeIO && !NativeIO.__mergerinoPatched) {
            const PatchedIO = function (cb, options) {
                const observer = new NativeIO(cb, options);
                const origObserve = observer.observe.bind(observer);
                observer.observe = function (target) {
                    origObserve(target);
                    setTimeout(function () {
                        try {
                            const rect = target.getBoundingClientRect
                                ? target.getBoundingClientRect()
                                : { x: 0, y: 0, width: 100, height: 100,
                                    top: 0, left: 0, right: 100, bottom: 100 };
                            cb([{
                                target: target,
                                isIntersecting: true,
                                intersectionRatio: 1.0,
                                time: (performance && performance.now())
                                    || Date.now(),
                                boundingClientRect: rect,
                                intersectionRect: rect,
                                rootBounds: null,
                            }], observer);
                        } catch (e) { /* ignore */ }
                    }, 0);
                };
                return observer;
            };
            PatchedIO.__mergerinoPatched = true;
            PatchedIO.prototype = NativeIO.prototype;
            window.IntersectionObserver = PatchedIO;
        }
    } catch (e) { /* ignore */ }
)JS"
LR"JS(
    try {
        // Force document.hasFocus() to return true. The dom-snapshot
        // diagnostic showed focus=false in the hidden host, and that is
        // a likely gating signal for the chat-message subscription
        // (chat container mounts but stays empty).
        const trueFn = function () { return true; };
        document.hasFocus = trueFn;
        Object.defineProperty(document, 'hidden', {
            configurable: true, get: function () { return false; },
        });
        Object.defineProperty(document, 'webkitHidden', {
            configurable: true, get: function () { return false; },
        });
        Object.defineProperty(document, 'visibilityState', {
            configurable: true, get: function () { return 'visible'; },
        });
        Object.defineProperty(document, 'webkitVisibilityState', {
            configurable: true, get: function () { return 'visible'; },
        });
        // Fire one synthetic visibilitychange so listeners attached at
        // boot see "visible" even if they polled the value once and
        // cached it. Subsequent real events from the runtime are still
        // forwarded normally.
        document.dispatchEvent(new Event('visibilitychange'));
    } catch (e) { /* if any of these fail, fall through to WS hook */ }

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
        // Forward a lightweight ws-detect for every WebSocket open so we
        // can see if chat content is on a different URL than im-ws.
        // Heartbeat-only frames on the known endpoint suggest the page
        // may open a second socket for actual chat events.
        post({ kind: 'ws-detect', url: u });
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

    // Install fetch hook BEFORE DOM scraping so a bug in the DOM section
    // can't break live detection (which depends on intercepting
    // /webcast/room/check_alive responses).
    const isRoomInfoRequest = function (u) {
        return /webcast\/room\/(enter|info|check_alive)/i.test(String(u));
    };

    let fetchSeenCount = 0;
    let xhrSeenCount = 0;
    let mcsResponseCount = 0;
    let genericResponseCount = 0;
    let xhrResponseCount = 0;
    const NativeFetch = window.fetch;
    if (typeof NativeFetch === 'function' && !NativeFetch.__mergerinoPatched) {
        const PatchedFetch = function (input, init) {
            let u = '';
            try {
                u = typeof input === 'string' ? input : (input && input.url) || '';
            } catch (e) { /* ignore */ }
            const promise = NativeFetch.apply(this, arguments);
            try {
                const us = String(u);
                if (us && fetchSeenCount < 80) {
                    fetchSeenCount++;
                    post({ kind: 'http-detect', url: us });
                }
                // Capture response bodies for any URL that might carry
                // chat data. Broad net so we don't miss it again.
                if (us && /event\/feed|webmssdk\/web|im_proxy|\/chat|\/message/i.test(us)
                    && genericResponseCount < 30) {
                    genericResponseCount++;
                    promise.then(function (resp) {
                        return resp.clone().text();
                    }).then(function (text) {
                        post({
                            kind: 'generic-response',
                            url: us,
                            length: text.length,
                            head: text.slice(0, 2000),
                        });
                    }).catch(function () { /* ignore */ });
                }
                // Capture response bodies for the high-cadence MCS poll -
                // 700ms polling implies real-time data delivery; previously
                // unseen because the URL doesn't match webcast/im patterns.
                if (us && /mcs[^/]*\.tiktokw\.eu\/v1\/list/i.test(us)) {
                    promise.then(function (resp) {
                        return resp.clone().text();
                    }).then(function (text) {
                        if (mcsResponseCount < 5) {
                            mcsResponseCount++;
                            post({
                                kind: 'mcs-response',
                                url: us,
                                length: text.length,
                                head: text.slice(0, 2000),
                            });
                        }
                    }).catch(function () { /* ignore */ });
                }
            } catch (e) { /* ignore */ }
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
)JS"
LR"JS(
    // Hook XMLHttpRequest. Most webcast subsystems still use XHR.
    // We capture response bodies for the high-cadence MCS poll which
    // is the suspected chat-data channel.
    try {
        const NativeXhrOpen = XMLHttpRequest.prototype.open;
        if (NativeXhrOpen && !NativeXhrOpen.__mergerinoPatched) {
            const PatchedXhrOpen = function (method, url) {
                try {
                    const us = String(url || '');
                    this.__mergerinoUrl = us;
                    this.__mergerinoMethod = method;
                    if (us && xhrSeenCount < 80) {
                        xhrSeenCount++;
                        post({ kind: 'xhr-detect', method: method, url: us });
                    }
                    if (us && /event\/feed|webmssdk\/web|im_proxy|\/chat|\/message/i.test(us)) {
                        const xhr = this;
                        this.addEventListener('load', function () {
                            try {
                                if (xhrResponseCount >= 30) { return; }
                                xhrResponseCount++;
                                const txt = String(xhr.responseText || '');
                                post({
                                    kind: 'xhr-response',
                                    url: us,
                                    length: txt.length,
                                    head: txt.slice(0, 2000),
                                });
                            } catch (e) { /* ignore */ }
                        });
                    }
                } catch (e) { /* ignore */ }
                return NativeXhrOpen.apply(this, arguments);
            };
            PatchedXhrOpen.__mergerinoPatched = true;
            XMLHttpRequest.prototype.open = PatchedXhrOpen;
        }
    } catch (e) { /* ignore */ }

    // Detect any Worker / SharedWorker creation. Workers have their
    // own fetch / WebSocket context which our window-level overrides
    // do NOT touch - if TikTok delivers chat through a worker, that's
    // why we never see it on the main thread.
    try {
        const NativeWorker = window.Worker;
        if (NativeWorker && !NativeWorker.__mergerinoPatched) {
            const PatchedWorker = function (url, options) {
                try { post({ kind: 'worker-detect', url: String(url || '') }); }
                catch (e) { /* ignore */ }
                return new NativeWorker(url, options);
            };
            PatchedWorker.__mergerinoPatched = true;
            window.Worker = PatchedWorker;
        }
        const NativeShared = window.SharedWorker;
        if (NativeShared && !NativeShared.__mergerinoPatched) {
            const PatchedShared = function (url, options) {
                try { post({ kind: 'shared-worker-detect', url: String(url || '') }); }
                catch (e) { /* ignore */ }
                return new NativeShared(url, options);
            };
            PatchedShared.__mergerinoPatched = true;
            window.SharedWorker = PatchedShared;
        }
        if (navigator.serviceWorker) {
            post({
                kind: 'service-worker-status',
                hasController: !!navigator.serviceWorker.controller,
                controllerUrl: (navigator.serviceWorker.controller
                    && navigator.serviceWorker.controller.scriptURL) || '',
            });
        }
    } catch (e) { /* ignore */ }

    // Hook EventSource (Server-Sent Events) - last possible chat
    // delivery mechanism we haven't checked.
    try {
        const NativeEventSource = window.EventSource;
        if (NativeEventSource && !NativeEventSource.__mergerinoPatched) {
            const PatchedEventSource = function (url, init) {
                try { post({ kind: 'sse-detect', url: String(url || '') }); }
                catch (e) { /* ignore */ }
                return new NativeEventSource(url, init);
            };
            PatchedEventSource.__mergerinoPatched = true;
            PatchedEventSource.prototype = NativeEventSource.prototype;
            window.EventSource = PatchedEventSource;
        }
    } catch (e) { /* ignore */ }
)JS"
LR"JS(
    // DOM-scraping fallback for chat. The webcast WebSocket on a hidden
    // host only delivers heartbeat / state-poll frames (verified via
    // 1680-byte uniform-size dumps), so the actual chat content has to
    // be read from the rendered chat list instead. Substring class
    // selectors used because TikTok's React tree uses styled-components -
    // the css-XXXX prefix hash rotates per build, but the human-readable
    // component name suffix stays stable for months.
    //
    // Wrapped in try/catch so any selector / DOM-API failure can't
    // propagate up and undo the WS+fetch hooks installed above.
    try {
        // Diagnostic: post snapshots every 5s for 30s so we can see
        // whether the React tree grows over time or stays stuck.
        // Each snapshot also dumps data-e2e attribute values (TikTok's
        // primary stable hook) so we can find chat-related attrs even
        // when class names are rotated emotion hashes.
        let snapTick = 0;
        const snapshotIntervalId = setInterval(function () {
            try {
                snapTick++;
                const body = document.body;
                const all = body ? body.querySelectorAll('*') : [];
                const chat = document.querySelectorAll(
                    '[class*="Chat"],[class*="chat"]');
                const comment = document.querySelectorAll(
                    '[class*="Comment"],[class*="comment"]');
                const e2eEls = document.querySelectorAll('[data-e2e]');
                const e2eValues = {};
                for (let i = 0; i < e2eEls.length; i++) {
                    const v = e2eEls[i].getAttribute('data-e2e') || '';
                    e2eValues[v] = (e2eValues[v] || 0) + 1;
                }
                const e2eList = [];
                for (const k in e2eValues) {
                    e2eList.push(k + '(' + e2eValues[k] + ')');
                    if (e2eList.length >= 25) { break; }
                }
                // Peek inside the chat container so we can see what
                // structure new messages will have.
                const container = document.querySelector(
                    "[data-e2e='live-chat-container']");
                const containerHtml = container
                    ? (container.outerHTML || '').slice(-3000)
                    : '';
                const childCount = container ? container.children.length : -1;
                const descendants = container
                    ? container.querySelectorAll('*').length : -1;
                post({
                    kind: 'dom-snapshot',
                    tick: snapTick,
                    readyState: document.readyState,
                    bodyExists: !!body,
                    elementCount: all.length,
                    chatCount: chat.length,
                    commentCount: comment.length,
                    dataE2eCount: e2eEls.length,
                    e2eValues: e2eList,
                    innerWidth: window.innerWidth,
                    innerHeight: window.innerHeight,
                    visibilityState: document.visibilityState,
                    hasFocus: document.hasFocus && document.hasFocus(),
                    containerChildren: childCount,
                    containerDescendants: descendants,
                    containerHtml: containerHtml,
                });
                if (snapTick >= 6) { clearInterval(snapshotIntervalId); }
            } catch (e) {
                post({ kind: 'dom-snapshot-error', message: String(e) });
                clearInterval(snapshotIntervalId);
            }
        }, 5000);

        // Selectors based on dom-snapshot enumeration of TikTok's
        // current data-e2e attributes (2026-05-05). data-e2e is the
        // stable hook because TikTok's CI emits it for QA tests, while
        // class names are emotion-hashed and rotate per build.
        const SEL_LIST = "[data-e2e='live-chat-container']";
        const SEL_ROW = "[data-e2e='message'],[data-e2e='chat-message']";
        const SEL_USER = "[data-e2e='message-owner-name'],[data-e2e='chat-message-name']";
        const SEL_TEXT = "[data-e2e='message-content'],[data-e2e='chat-message-text']";

        const seenRows = new WeakSet();
        let chatList = null;
        let chatObserver = null;
        let surveyPosted = false;

        const cleanText = function (s) {
            return (s || '').replace(/\s+/g, ' ').trim();
        };

        const processRow = function (node) {
            if (!(node instanceof Element) || seenRows.has(node)) { return; }
            const userEl = node.matches && node.matches(SEL_ROW)
                ? node.querySelector(SEL_USER) : null;
            const textEl = node.matches && node.matches(SEL_ROW)
                ? node.querySelector(SEL_TEXT) : null;
            if (!userEl && !textEl) {
                const inner = node.querySelectorAll
                    ? node.querySelectorAll(SEL_ROW) : [];
                inner.forEach(processRow);
                return;
            }
            const user = cleanText(userEl ? userEl.textContent : '');
            const text = cleanText(textEl ? textEl.textContent : '');
            if (!user || !text) { return; }
            seenRows.add(node);
            post({ kind: 'dom-chat', user: user, text: text, ts: Date.now() });
        };

        const attachToList = function () {
            try {
                if (chatList && document.contains(chatList)) { return; }
                const list = document.querySelector(SEL_LIST);
                if (!list) {
                    if (!surveyPosted && document.body) {
                        const candidates = [];
                        const all = document.querySelectorAll(
                            '[class*="Chat"],[class*="Comment"]');
                        for (let i = 0; i < Math.min(all.length, 12); i++) {
                            candidates.push(all[i].className || '');
                        }
                        if (candidates.length > 0) {
                            surveyPosted = true;
                            post({ kind: 'dom-survey', candidates: candidates });
                        }
                    }
                    return;
                }
                chatList = list;
                if (chatObserver) { chatObserver.disconnect(); }
                chatObserver = new MutationObserver(function (muts) {
                    for (let i = 0; i < muts.length; i++) {
                        const added = muts[i].addedNodes;
                        for (let j = 0; j < added.length; j++) {
                            processRow(added[j]);
                        }
                    }
                });
                chatObserver.observe(list, { childList: true, subtree: true });
                post({ kind: 'dom-chat-attached',
                       count: list.querySelectorAll(SEL_ROW).length });
            } catch (e) { /* ignore - retry on next poll */ }
        };

        // Poll until the chat list mounts. setInterval is cheap (every
        // 1.5s) and avoids the cost of a tree-wide MutationObserver,
        // which fires for every node mutation on a busy SPA.
        const pollId = setInterval(function () {
            attachToList();
            if (chatList) { clearInterval(pollId); }
        }, 1500);
        if (document.readyState !== 'loading') {
            attachToList();
        } else {
            document.addEventListener('DOMContentLoaded', attachToList);
        }
    } catch (e) { /* DOM scraping fully optional - fall through */ }
})();
)JS";

}  // namespace chatterino::tiktok
