/**
 * web_ui.cpp — on-device web UI for the A1Keyer.
 *
 * Routes:
 *   GET  /              — dark, mobile-responsive single-page app
 *                         (HTML/CSS/JS as a raw string literal below).
 *   POST /api/settings  — partial settings patch. Each present key
 *                         applies via the matching MorseModel setter,
 *                         then mirrors the keyboard-UI NVS write
 *                         (see src/main.cpp:1002-1044 for the original
 *                         pattern this replicates).
 *   POST /api/memory    — write one memory slot (slot 0..9, text ≤80).
 *                         Persists the entire bank via memoryBankSave
 *                         (matches the keyboard MEMORY_EDIT flow).
 *
 * Body is `#if ENABLE_WIFI_DEBUG`. When the flag is off the module
 * compiles to two no-op stubs and the WebServer library is not pulled in.
 *
 * JSON in: tiny ad-hoc key/value scanner — no ArduinoJson (matches
 * console_server.cpp's deliberate avoidance of the dependency).
 */
#include "web_ui.h"

#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)

#include <Arduino.h>
#include <WebServer.h>

#include <Preferences.h>

#include "http_server.h"
#include "display_model.h"
#include "network_manager.h"
#include "memory_store.h"
#include "Log.h"
#include "winkey.h"
#include "tx_buffer.h"

// Forward declaration for the inline HTML asset defined at file scope
// near the bottom of this file. File-scope declaration so the linker
// sees a single global symbol even though the consumer (handleRoot)
// lives inside the anonymous namespace below.
extern const char kIndexHtml[];

// ─── TX-buffer input validation ───────────────────────────────────────────
//
// ASCII printable + a small set of accepted punctuation. Multi-byte UTF-8,
// control characters, and newlines are rejected — MorseEncoder handles
// neither. Newlines would also confuse the JSON web UI's single-line
// display. The check is deliberately permissive on punctuation so common
// CW macros ("CQ CQ DE W1AW K", "5NN/B", "<AR>", "<SK>") go through.
namespace {
bool isValidTxChar(unsigned char c) {
    // Standard printable ASCII.
    if (c >= 0x20 && c <= 0x7E) return true;
    return false;
}

bool isValidTxText(const char* text) {
    if (!text) return false;
    for (const char* p = text; *p; ++p) {
        if (!isValidTxChar((unsigned char)*p)) return false;
    }
    return true;
}
}  // namespace

namespace {

// ─── JSON helpers (mirror the console_server.cpp style) ──────────────────

void jsonEscape(String& dst, const char* src) {
    if (!src) return;
    while (*src) {
        char c = *src++;
        switch (c) {
            case '"':  dst += "\\\""; break;
            case '\\': dst += "\\\\"; break;
            case '\n': dst += "\\n";  break;
            case '\r': dst += "\\r";  break;
            case '\t': dst += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    dst += buf;
                } else {
                    dst += c;
                }
        }
    }
}

// ─── POST body parsing ──────────────────────────────────────────────────
//
// Tiny single-pass scanner. Walks the body looking for `"key":value`
// pairs. Value types: string (quoted), number (digits + optional
// minus + dot), bool (true|false|null). Stops at the first parse
// error and returns the number of recognized keys. Used for
// best-effort settings patches — unknown keys are ignored (the server
// doesn't have to know about every client-side field).
//
// To avoid allocating temporaries, each value is written into a
// caller-provided buffer via callbacks. Keeps the dependency
// footprint small and matches the project's snprintf-into-String
// style throughout console_server.cpp.

struct ParsedBody {
    bool    hasWpm                  = false;
    int     wpm                     = 0;

    bool    hasFrequency            = false;
    int     frequency               = 0;

    bool    hasVolume               = false;
    int     volume                  = 0;

    bool    hasPolarityReversed     = false;
    bool    polarityReversed        = false;

    bool    hasRadioKeyingEnabled   = false;
    bool    radioKeyingEnabled      = false;

    bool    hasKeyerType            = false;
    bool    keyerTypeIsStraight     = false;  // false = paddle, true = straight

    bool    hasWinkeyMode           = false;
    bool    winkeyMode              = false;

    bool    hasSlot                 = false;
    int     slot                    = -1;

    bool    hasText                 = false;
    char    text[kMemLen]           = {0};
};

// Skip whitespace.
const char* skipWs(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

// Read `"key"` starting at `*pp` (which points at the opening quote).
// On success, advances `*pp` past the closing quote and returns true.
bool parseKey(const char** pp, char* out, size_t cap) {
    const char* p = *pp;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            // For our purposes we don't need full escape handling —
            // the keys we recognize contain no escapes.
            if (i + 1 < cap) out[i++] = *(p + 1);
            p += 2;
        } else {
            if (i + 1 < cap) out[i++] = *p;
            ++p;
        }
    }
    if (*p != '"') return false;
    out[i] = '\0';
    ++p;
    *pp = p;
    return true;
}

// Read a string value into `out` (capacity `cap`). Advances `*pp` past
// the closing quote. The caller is expected to have already peeked for
// the opening quote.
void parseStringValue(const char** pp, char* out, size_t cap) {
    const char* p = *pp;
    // p points at opening quote.
    if (*p != '"') { out[0] = '\0'; return; }
    ++p;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            char esc = *(p + 1);
            char resolved = esc;
            switch (esc) {
                case 'n': resolved = '\n'; break;
                case 'r': resolved = '\r'; break;
                case 't': resolved = '\t'; break;
                case '\\':
                case '"': resolved = esc; break;
                default:  resolved = esc; break;
            }
            if (i + 1 < cap) out[i++] = resolved;
            p += 2;
        } else {
            if (i + 1 < cap) out[i++] = *p;
            ++p;
        }
    }
    if (i < cap) out[i] = '\0';
    else out[cap - 1] = '\0';
    if (*p == '"') ++p;
    *pp = p;
}

// Parse a numeric value. Returns the value via the out parameter.
// Advances `*pp` to the first non-numeric, non-`,`, non-`}`, non-`]`
// character. Robust enough for our JSON-shaped payloads.
bool parseNumber(const char** pp, double* out) {
    const char* p = skipWs(*pp);
    char* endp = nullptr;
    double v = strtod(p, &endp);
    if (endp == p) return false;
    *out = v;
    *pp = endp;
    return true;
}

bool parseBool(const char** pp, bool* out) {
    const char* p = skipWs(*pp);
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        *pp = p + 4;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        *pp = p + 5;
        return true;
    }
    return false;
}

// Match key against an expected literal. Returns true and advances `*pp`
// past the colon if matched.
bool expectKey(const char** pp, const char* expected) {
    const char* p = skipWs(*pp);
    size_t n = strlen(expected);
    if (strncmp(p, expected, n) != 0) return false;
    p += n;
    p = skipWs(p);
    if (*p != ':') return false;
    *pp = p + 1;
    return true;
}

void parseObject(const char** pp, ParsedBody& body) {
    const char* p = skipWs(*pp);
    if (*p != '{') return;
    ++p;
    while (*p) {
        p = skipWs(p);
        if (*p == '}') { ++p; break; }
        if (*p != '"') return;
        char key[32];
        if (!parseKey(&p, key, sizeof(key))) return;
        p = skipWs(p);
        if (*p != ':') return;
        ++p;
        p = skipWs(p);

        if      (strcmp(key, "wpm") == 0)                { double v; if (parseNumber(&p, &v)) { body.hasWpm = true; body.wpm = (int)v; } }
        else if (strcmp(key, "frequency") == 0)          { double v; if (parseNumber(&p, &v)) { body.hasFrequency = true; body.frequency = (int)v; } }
        else if (strcmp(key, "volume") == 0)             { double v; if (parseNumber(&p, &v)) { body.hasVolume = true; body.volume = (int)v; } }
        else if (strcmp(key, "polarityReversed") == 0)   { bool v;    if (parseBool  (&p, &v)) { body.hasPolarityReversed = true; body.polarityReversed = v; } }
        else if (strcmp(key, "radioKeyingEnabled") == 0) { bool v;    if (parseBool  (&p, &v)) { body.hasRadioKeyingEnabled = true; body.radioKeyingEnabled = v; } }
        else if (strcmp(key, "winkeyMode") == 0)         { bool v;    if (parseBool  (&p, &v)) { body.hasWinkeyMode = true; body.winkeyMode = v; } }
        else if (strcmp(key, "keyerType") == 0)          { char v[16]; parseStringValue(&p, v, sizeof(v)); body.hasKeyerType = true; body.keyerTypeIsStraight = (strcmp(v, "straight") == 0); }
        else if (strcmp(key, "slot") == 0)               { double v; if (parseNumber(&p, &v)) { body.hasSlot = true; body.slot = (int)v; } }
        else if (strcmp(key, "text") == 0)               { parseStringValue(&p, body.text, sizeof(body.text)); body.hasText = true; }
        else {
            // Skip unknown value — number, bool, or string. Cheaper to
            // bail on truly unknown content than to grow the parser.
            if      (*p == '"') { char tmp[64]; parseStringValue(&p, tmp, sizeof(tmp)); }
            else if (*p == 't' || *p == 'f') { bool tmp; parseBool(&p, &tmp); }
            else                { double tmp; parseNumber(&p, &tmp); }
        }

        p = skipWs(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; break; }
        // Tolerate trailing junk between pairs
        if (*p == '\0') return;
    }
    *pp = p;
}

// ─── NVS persistence (mirror of main.cpp:1002-1044) ────────────────────

bool persistSetting(const char* key, int value) {
    Preferences prefs;
    if (!prefs.begin("morse", false)) return false;
    prefs.putInt(key, value);
    prefs.end();
    return true;
}

bool persistSetting(const char* key, bool value) {
    Preferences prefs;
    if (!prefs.begin("morse", false)) return false;
    prefs.putBool(key, value);
    prefs.end();
    return true;
}

bool persistKeyType(const char* value) {
    Preferences prefs;
    if (!prefs.begin("morse", false)) return false;
    prefs.putString("keytype", value);
    prefs.end();
    return true;
}

// ─── Handlers ───────────────────────────────────────────────────────────

// Serve the inline dark HTML/CSS/JS app. The asset is a raw string
// literal below in this file — kept as a single constant so the
// compiler folds the bytes into a single contiguous segment and we
// hand `WebServer::send` one String.
void handleRoot() {
    String body;
    body.reserve(strlen(kIndexHtml) + 16);
    body += kIndexHtml;
    HttpServer::server().send(200, "text/html; charset=utf-8", body);
}

// POST /api/settings — apply a partial patch and persist each touched
// field to NVS. Returns { ok:true, applied:[ ... ] } on success or
// { ok:false, error:"..." } otherwise.
void handleApiSettings() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }
    String raw = server.arg("plain");
    if (raw.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
        return;
    }

    ParsedBody body;
    const char* p = raw.c_str();
    parseObject(&p, body);
    if (!body.hasWpm && !body.hasFrequency && !body.hasVolume &&
        !body.hasPolarityReversed && !body.hasRadioKeyingEnabled &&
        !body.hasWinkeyMode && !body.hasKeyerType) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unrecognized payload\"}");
        return;
    }

    auto& model = MorseModel::instance();
    String applied;
    applied.reserve(96);
    applied += '[';

    if (body.hasWpm) {
        model.setWPM(body.wpm);
        persistSetting("wpm", body.wpm);
        applied += "\"wpm\",";
    }
    if (body.hasFrequency) {
        model.setFrequency((float)body.frequency);
        persistSetting("freq", body.frequency);
        applied += "\"frequency\",";
    }
    if (body.hasVolume) {
        model.setVolume(body.volume);
        persistSetting("vol", body.volume);
        applied += "\"volume\",";
    }
    if (body.hasPolarityReversed) {
        model.setPolarityReversed(body.polarityReversed);
        persistSetting("polarity", body.polarityReversed);
        applied += "\"polarityReversed\",";
    }
    if (body.hasRadioKeyingEnabled) {
        model.setRadioKeyingEnabled(body.radioKeyingEnabled);
        persistSetting("keying", body.radioKeyingEnabled);
        applied += "\"radioKeyingEnabled\",";
    }
    if (body.hasKeyerType) {
        model.setKeyerType(body.keyerTypeIsStraight ? KeyerType::STRAIGHT : KeyerType::PADDLE);
        persistKeyType(body.keyerTypeIsStraight ? "straight" : "paddle");
        applied += "\"keyerType\",";
    }
    if (body.hasWinkeyMode) {
        // Session-only by design (the on-device 'D' key does not persist).
        model.setWinkeyMode(body.winkeyMode);
        applied += "\"winkeyMode\",";
    }

    if (applied.length() > 1) applied[applied.length() - 1] = ']';
    else                       applied += ']';

    String reply;
    reply.reserve(applied.length() + 16);
    reply += "{\"ok\":true,\"applied\":";
    reply += applied;
    reply += '}';
    server.send(200, "application/json", reply);
}

// POST /api/memory — update one slot in the MorseModel mirror and
// persist the entire bank to NVS (matches main.cpp's keyboard flow).
void handleApiMemory() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }
    String raw = server.arg("plain");
    ParsedBody body;
    const char* p = raw.c_str();
    parseObject(&p, body);
    if (!body.hasSlot || body.slot < 0 || body.slot >= (int)MorseModel::kMemSlots) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"slot must be 0..9\"}");
        return;
    }
    if (!body.hasText) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing text\"}");
        return;
    }

    auto& model = MorseModel::instance();
    // Load the current bank, apply the edit, save back — this matches
    // the load-modify-save dance used in the keyboard flow and ensures
    // all 10 slots stay consistent on the device even if other
    // writers (the keyboard UI) edit them concurrently.
    MemoryBank bank;
    memoryBankLoad(bank);
    memCopyStr(bank.slot[body.slot], kMemLen, body.text);
    model.copyMemoryBank(bank);
    model.setMemory((uint8_t)body.slot, body.text);
    bool saved = memoryBankSave(bank);

    char reply[96];
    int n = snprintf(reply, sizeof(reply),
                     "{\"ok\":%s,\"slot\":%d,\"len\":%u,\"saved\":%s}",
                     saved ? "true" : "false",
                     body.slot,
                     (unsigned)strlen(body.text),
                     saved ? "true" : "false");
    server.send(saved ? 200 : 500, "application/json", String(reply, n));
}

// POST /api/play — playback of one memory slot via the same path the
// keyboard's digit-0..9 handler uses (src/main.cpp:940-961). Returns
// { ok:true, slot } on success, { ok:false, error } on bad input.
void handleApiPlay() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }
    String raw = server.arg("plain");
    ParsedBody body;
    const char* p = raw.c_str();
    parseObject(&p, body);
    if (!body.hasSlot || body.slot < 0 || body.slot >= (int)MorseModel::kMemSlots) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"slot must be 0..9\"}");
        return;
    }
    const char* text = MorseModel::instance().getMemory((uint8_t)body.slot);
    if (!text || text[0] == '\0') {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"slot empty\"}");
        return;
    }
    Winkey::playLocalMemoryText(text);
    char reply[64];
    int n = snprintf(reply, sizeof(reply), "{\"ok\":true,\"slot\":%d}", body.slot);
    server.send(200, "application/json", String(reply, n));
}

// ─── TX-buffer endpoints ───────────────────────────────────────────────────
//
// All five endpoints below drive MorseModel::_txBuffer — the same buffer
// the WinKey host's ADMIN_TX_BUFFER_LOAD/START/CLEAR commands target.
// Session-only, no NVS persistence. See docs/tx_buffer.md § 4 and § 8.

// Helper: build a JSON snapshot of the TX-buffer state for replies.
// Includes the same fields /state emits so the client can re-render
// without an extra round-trip after a successful mutation.
void sendTxStateReply(int httpCode, bool ok, const char* error) {
    auto& server = HttpServer::server();
    auto& m = MorseModel::instance();
    String reply;
    reply.reserve(384);
    reply += "{\"ok\":";
    reply += ok ? "true" : "false";
    if (!ok) {
        reply += ",\"error\":\"";
        jsonEscape(reply, error ? error : "unknown");
        reply += '"';
    }
    reply += ",\"txLen\":";
    reply += m.txLen();
    reply += ",\"txSent\":";
    reply += m.txSent();
    reply += ",\"txHead\":";
    reply += m.txHead();
    reply += ",\"txChunkStart\":";
    reply += m.txChunkStart();
    reply += ",\"txChunkLen\":";
    reply += m.txChunkLen();
    reply += ",\"txActive\":";
    reply += m.txActive() ? "true" : "false";
    reply += ",\"txEditableStart\":";
    reply += m.txEditableStart();
    reply += ",\"txBuffer\":\"";
    jsonEscape(reply, m.txBuffer());
    reply += "\"}";
    server.send(httpCode, "application/json", reply);
}

// POST /api/tx/text — full-buffer replace. Used on textarea blur and
// on "Clear + retype" flows. Resets txSent=0 and clears any active
// session.
void handleApiTxText() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }
    String raw = server.arg("plain");
    // Extract the "text" field via a one-off scan. Avoids forcing a
    // generic parser upgrade for this single endpoint.
    int keyIdx = raw.indexOf("\"text\"");
    if (keyIdx < 0) {
        sendTxStateReply(400, false, "missing text");
        return;
    }
    int colon = raw.indexOf(':', keyIdx);
    if (colon < 0) {
        sendTxStateReply(400, false, "missing text");
        return;
    }
    int q1 = raw.indexOf('"', colon);
    if (q1 < 0) {
        sendTxStateReply(400, false, "missing text");
        return;
    }
    int q2 = raw.indexOf('"', q1 + 1);
    if (q2 < 0) {
        sendTxStateReply(400, false, "unterminated text");
        return;
    }
    String text = raw.substring(q1 + 1, q2);

    if (text.length() > (int)(MorseModel::kTxBufLen - 1)) {
        sendTxStateReply(400, false, "text too long (max 255)");
        return;
    }
    if (!isValidTxText(text.c_str())) {
        sendTxStateReply(400, false, "invalid characters (ASCII printable only)");
        return;
    }

    MorseModel::instance().setTxText(text.c_str());
    sendTxStateReply(200, true, nullptr);
    Log::info("[TX] /api/tx/text: len=%d", text.length());
}

// POST /api/tx/edit — keystroke-level edits. Body: {op:"append"|"backspace"|"clear", c?:"X"}.
// append + c: appends the single character. backspace: deletes the last
// editable char. clear: wipes everything.
void handleApiTxEdit() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }
    String raw = server.arg("plain");

    // Extract "op" field.
    int opIdx = raw.indexOf("\"op\"");
    if (opIdx < 0) {
        sendTxStateReply(400, false, "missing op");
        return;
    }
    int opColon = raw.indexOf(':', opIdx);
    if (opColon < 0) {
        sendTxStateReply(400, false, "missing op");
        return;
    }
    int opQ1 = raw.indexOf('"', opColon);
    if (opQ1 < 0) {
        sendTxStateReply(400, false, "missing op");
        return;
    }
    int opQ2 = raw.indexOf('"', opQ1 + 1);
    if (opQ2 < 0) {
        sendTxStateReply(400, false, "unterminated op");
        return;
    }
    String op = raw.substring(opQ1 + 1, opQ2);

    auto& model = MorseModel::instance();

    if (op == "append") {
        // Extract "c" field.
        int cIdx = raw.indexOf("\"c\"");
        if (cIdx < 0) {
            sendTxStateReply(400, false, "append requires c");
            return;
        }
        int cColon = raw.indexOf(':', cIdx);
        if (cColon < 0) {
            sendTxStateReply(400, false, "append requires c");
            return;
        }
        int cQ1 = raw.indexOf('"', cColon);
        if (cQ1 < 0) {
            sendTxStateReply(400, false, "append requires c");
            return;
        }
        int cQ2 = raw.indexOf('"', cQ1 + 1);
        if (cQ2 < 0 || cQ2 - cQ1 - 1 != 1) {
            sendTxStateReply(400, false, "c must be a single char");
            return;
        }
        char c = raw.charAt(cQ1 + 1);
        if (!isValidTxChar((unsigned char)c)) {
            sendTxStateReply(400, false, "invalid character");
            return;
        }
        bool ok = model.appendTxChar(c);
        if (!ok) {
            sendTxStateReply(400, false, "buffer full (max 255)");
            return;
        }
        sendTxStateReply(200, true, nullptr);
        return;
    }
    if (op == "backspace") {
        bool removed = model.backspaceTx();
        if (!removed) {
            sendTxStateReply(400, false, "nothing to backspace");
            return;
        }
        sendTxStateReply(200, true, nullptr);
        return;
    }
    if (op == "clear") {
        model.clearTx();
        sendTxStateReply(200, true, nullptr);
        return;
    }
    sendTxStateReply(400, false, "unknown op");
}

// POST /api/tx/start — arm playback. No-op if already active or nothing to send.
void handleApiTxStart() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    auto& m = MorseModel::instance();
    if (m.txActive()) {
        sendTxStateReply(400, false, "already active");
        return;
    }
    if (m.txSent() >= m.txLen()) {
        sendTxStateReply(400, false, "nothing to send");
        return;
    }
    TxBuffer::beginSession();
    sendTxStateReply(200, true, nullptr);
    Log::info("[TX] /api/tx/start: sent=%zu len=%zu", m.txSent(), m.txLen());
}

// POST /api/tx/stop — cancel in-flight playback. Pending preserved.
void handleApiTxStop() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    MorseModel::instance().stopTx();
    sendTxStateReply(200, true, nullptr);
    Log::info("[TX] /api/tx/stop");
}

// POST /api/tx/clear — wipe everything.
void handleApiTxClear() {
    auto& server = HttpServer::server();
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }
    MorseModel::instance().clearTx();
    sendTxStateReply(200, true, nullptr);
    Log::info("[TX] /api/tx/clear");
}

}  // namespace

void WebUI::begin(uint16_t port) {
    (void)port;
    using namespace HttpServer;
    addRoute((uint16_t)HTTP_GET,  "/",              handleRoot);
    addRoute((uint16_t)HTTP_POST, "/api/settings",  handleApiSettings);
    addRoute((uint16_t)HTTP_POST, "/api/memory",    handleApiMemory);
    addRoute((uint16_t)HTTP_POST, "/api/play",      handleApiPlay);
    addRoute((uint16_t)HTTP_POST, "/api/tx/text",   handleApiTxText);
    addRoute((uint16_t)HTTP_POST, "/api/tx/edit",   handleApiTxEdit);
    addRoute((uint16_t)HTTP_POST, "/api/tx/start",  handleApiTxStart);
    addRoute((uint16_t)HTTP_POST, "/api/tx/stop",   handleApiTxStop);
    addRoute((uint16_t)HTTP_POST, "/api/tx/clear",  handleApiTxClear);
}

void WebUI::poll() {
    // Lazy start/stop is in HttpServer::poll(); the WebUI has no
    // additional per-tick work to do. Hook is here for symmetry with
    // ConsoleServer::poll() and as a place to add per-tick UI tasks
    // later (e.g. a WebSocket poll) without changing call sites.
}

// ─── Inline HTML/CSS/JS ────────────────────────────────────────────────
//
// Single-page dark app. No external assets. ~13 KB. Plain JS so we
// don't pull in a framework. Polling, optimistic UI, value-diffing
// to avoid touching unchanged DOM.
//
// The raw string literal intentionally has no escape conflicts with
// the surrounding C++ (it uses no backticks, no `R"delim(...)delim"`
// conflict characters — only single quotes inside JS strings are
// fine; we use double quotes throughout).

const char kIndexHtml[] = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>A1Keyer</title>
<style>
:root{
  --bg:#0e1116;--bg-card:#161b22;--fg:#e6edf3;--muted:#8b949e;
  --accent:#4ec9b0;--warn:#f0883e;--err:#f85149;--border:#30363d;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0;background:var(--bg);color:var(--fg);
  font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
header{position:sticky;top:0;z-index:10;display:flex;align-items:center;
  gap:12px;padding:12px 16px;background:var(--bg);border-bottom:1px solid var(--border)}
h1{margin:0;font-size:18px;font-weight:600}
.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;
  border:1px solid var(--border);border-radius:999px;font-size:13px;color:var(--muted)}
.pill.on{color:var(--accent);border-color:var(--accent)}
.pill.off{color:var(--err);border-color:var(--err)}
.dot{width:8px;height:8px;border-radius:999px;background:currentColor;display:inline-block}
main{max-width:1100px;margin:0 auto;padding:16px;display:grid;
  gap:16px;grid-template-columns:1fr}
@media(min-width:768px){main{grid-template-columns:1fr 1fr}}
.col-span-2{grid-column:1/-1}
.card{background:var(--bg-card);border:1px solid var(--border);
  border-radius:10px;padding:14px 16px}
.card h2{margin:0 0 10px;font-size:15px;font-weight:600;color:var(--fg);
  letter-spacing:.02em;text-transform:uppercase}
pre#decoded{margin:0;padding:10px 12px;background:var(--bg);border:1px solid var(--border);
  border-radius:6px;color:var(--fg);font:20px/1.4 ui-monospace,Menlo,Consolas,monospace;
  white-space:nowrap;overflow-x:auto;overflow-y:hidden;
  -webkit-overflow-scrolling:touch;scroll-behavior:smooth}
form{display:grid;gap:12px}
.row{display:grid;gap:6px}
.row label{display:flex;justify-content:space-between;align-items:center;font-size:13px;color:var(--muted)}
.row .val{color:var(--fg);font-variant-numeric:tabular-nums}
input[type=number],input[type=text],select{
  width:100%;min-height:44px;padding:8px 10px;background:var(--bg);
  color:var(--fg);border:1px solid var(--border);border-radius:6px;font:inherit}
input[type=range]{width:100%}
input:focus,select:focus{outline:none;border-color:var(--accent);
  box-shadow:0 0 0 2px rgba(78,201,176,.25)}
input[type=checkbox]{width:22px;height:22px;accent-color:var(--accent)}
button{min-height:44px;padding:8px 14px;background:var(--bg);
  color:var(--fg);border:1px solid var(--border);border-radius:6px;
  font:inherit;cursor:pointer}
button:hover{border-color:var(--accent)}
button:active{transform:translateY(1px)}
button.primary{background:var(--accent);color:#0e1116;border-color:var(--accent);font-weight:600}
button.warn{background:var(--warn);color:#0e1116;border-color:var(--warn);font-weight:600}
.mem{display:grid;grid-template-columns:54px 1fr auto auto;gap:8px;align-items:center;
  margin-bottom:8px}
.mem button{min-width:64px}
.mem label{font-weight:600;color:var(--muted)}

/* TX buffer card — three regions side by side */
.tx-display{display:flex;align-items:center;gap:0;font:20px/1.4 ui-monospace,Menlo,Consolas,monospace;
  background:var(--bg);border:1px solid var(--border);border-radius:6px;
  padding:6px 10px;min-height:36px;overflow-x:auto;white-space:nowrap}
.tx-sent{color:var(--muted)}
.tx-flight{color:var(--accent);position:relative}
.tx-flight::after{content:"";position:absolute;left:0;right:0;bottom:-2px;
  height:2px;background:var(--accent);animation:blink 1s step-end infinite}
@keyframes blink{50%{opacity:0}}
.tx-pending{flex:1;min-width:80px;background:transparent;border:0;
  color:var(--fg);font:inherit;outline:none;padding:0}
.tx-pending::placeholder{color:var(--muted)}
.tx-controls{display:flex;gap:8px;align-items:center;margin-top:10px}
.tx-controls .hint{margin-left:auto}
.banner{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);
  background:var(--err);color:#fff;padding:10px 14px;border-radius:6px;
  box-shadow:0 4px 12px rgba(0,0,0,.4);font-size:14px;display:none}
.banner.show{display:block}
.hint{font-size:12px;color:var(--muted)}
</style>
</head>
<body>
<header>
  <h1>A1Keyer</h1>
  <span id="wifi" class="pill"><span class="dot"></span>Connecting…</span>
</header>
<main>
  <section class="card col-span-2" id="decodedCard">
    <h2>Live decode</h2>
    <pre id="decoded"></pre>
  </section>
  <section class="card col-span-2" id="txCard">
    <h2>TX buffer</h2>
    <div class="tx-display">
      <span id="txSent"   class="tx-sent"></span>
      <span id="txFlight" class="tx-flight"></span>
      <input id="txPending" type="text" class="tx-pending"
             maxlength="255" autocomplete="off" spellcheck="false"
             placeholder="Type to compose. Click TX to transmit." />
    </div>
    <div class="tx-controls">
      <button id="txStart" class="primary">TX</button>
      <button id="txStop"  class="warn" hidden>Stop</button>
      <button id="txClear">Clear</button>
      <span id="txStatus" class="hint"></span>
    </div>
  </section>
  <section class="card">
    <h2>Settings</h2>
    <form id="settings" autocomplete="off">
      <div class="row">
        <label>WPM <span class="val" id="val-wpm"></span></label>
        <input type="number" id="wpm" min="5" max="50" step="1">
      </div>
      <div class="row">
        <label>Sidetone (Hz) <span class="val" id="val-freq"></span></label>
        <input type="number" id="freq" min="300" max="900" step="10">
      </div>
      <div class="row">
        <label>Volume <span class="val" id="val-vol"></span></label>
        <input type="range" id="vol" min="0" max="100">
      </div>
      <div class="row">
        <label>Paddle polarity</label>
        <select id="polarity">
          <option value="false">Normal</option>
          <option value="true">Reversed</option>
        </select>
      </div>
      <div class="row">
        <label><input type="checkbox" id="keying"> Radio keying on (GPIO4)</label>
      </div>
      <div class="row">
        <label>Keyer type</label>
        <select id="keyerType">
          <option value="paddle">Iambic paddle</option>
          <option value="straight">Straight key</option>
        </select>
      </div>
      <div class="row">
        <label>WinKey mode</label>
        <select id="winkey">
          <option value="true">WinKey</option>
          <option value="false">Console</option>
        </select>
        <span class="hint">Session-only — not persisted to NVS.</span>
      </div>
    </form>
  </section>
  <section class="card">
    <h2>Memory (M0–M9)</h2>
    <div id="memList"></div>
  </section>
</main>
<div id="banner" class="banner">Disconnected — retrying…</div>
<script>
(function(){
  var $ = function(id){return document.getElementById(id);};
  var last = {};
  var errors = 0, bannerTimer = null;

  function showBanner(msg){
    var b = $("banner");
    b.textContent = msg;
    b.classList.add("show");
    clearTimeout(bannerTimer);
    bannerTimer = setTimeout(function(){b.classList.remove("show");}, 4000);
  }

  function patchValue(el, val){
    var s = String(val);
    if (el.value !== s) el.value = s;
  }
  function patchChecked(el, val){
    var on = !!val;
    if (el.checked !== on) el.checked = on;
  }

  function renderState(s){
    if (!s) return;
    patchValue($("wpm"), s.wpm);
    $("val-wpm").textContent = s.wpm;
    patchValue($("freq"), s.frequency);
    $("val-freq").textContent = s.frequency + " Hz";
    patchValue($("vol"), s.volume);
    $("val-vol").textContent = s.volume + "%";
    var pol = $("polarity"); patchValue(pol, String(!!s.polarityReversed));
    patchChecked($("keying"), s.radioKeyingEnabled);
    patchValue($("keyerType"), s.keyerType);
    patchValue($("winkey"), String(!!s.winkeyMode));
    var dec = $("decoded");
    if (dec.textContent !== s.decoded) dec.textContent = s.decoded || "";
    // Single-line preview — newest chars are appended to the right,
    // so keep the rightmost content in view.
    dec.scrollLeft = dec.scrollWidth;
    renderMemory(s.memory || []);
    renderTx(s);
    var wifi = $("wifi");
    if (s.wifiIP && s.wifiIP !== "0.0.0.0"){
      wifi.className = "pill on";
      // "AP" prefix when the device itself is the access point, so
      // the operator doesn't misread "Connected 192.168.73.1" as a
      // STA-mode DHCP lease. Either label is technically true; the
      // distinction is which side of the link the device is on.
      var tag = s.wifiMode === "ap" ? "AP " : "Connected ";
      wifi.innerHTML = '<span class="dot"></span>' + tag + s.wifiIP;
    } else {
      wifi.className = "pill off";
      wifi.innerHTML = '<span class="dot"></span>Offline';
    }
    last = s;
  }

  function renderMemory(arr){
    var list = $("memList");
    var need = arr.length;
    var have = list.children.length;
    if (have !== need){
      list.innerHTML = "";
      for (var i = 0; i < need; i++){
        var row = document.createElement("div");
        row.className = "mem";
        var lbl = document.createElement("label");
        lbl.textContent = "M" + i;
        var inp = document.createElement("input");
        inp.type = "text"; inp.maxLength = 80; inp.id = "mem-" + i;
        inp.value = arr[i] || "";
        // Track local edits so the periodic /state poll doesn't stamp
        // over typing-in-progress. Cleared on successful /api/memory
        // save (see bindMemory below).
        inp.dataset.dirty = "0";
        inp.addEventListener("input", function(ev){
          ev.target.dataset.dirty = "1";
        });
        var play = document.createElement("button");
        play.type = "button";
        play.textContent = "▶";
        play.title = "Play M" + i;
        play.dataset.playSlot = i;
        if (!arr[i]) play.disabled = true;
        var btn = document.createElement("button");
        btn.type = "button";
        btn.textContent = "Save";
        btn.dataset.slot = i;
        row.appendChild(lbl); row.appendChild(inp);
        row.appendChild(play); row.appendChild(btn);
        list.appendChild(row);
      }
    } else {
      for (var j = 0; j < need; j++){
        var f = $("mem-" + j);
        if (f) {
          // Preserve in-progress edits: the device hasn't seen them
          // yet (we haven't POSTed), so the polled state will still
          // match the pre-edit value and we'd otherwise wipe the
          // user's typing every 500 ms.
          if (f.dataset.dirty !== "1" && f.value !== (arr[j] || "")) {
            f.value = arr[j] || "";
          }
          // If the server caught up to what the user has typed
          // (e.g. after Save), drop the dirty flag so future polls
          // can sync again.
          if (f.dataset.dirty === "1" && f.value === (arr[j] || "")) {
            f.dataset.dirty = "0";
          }
        }
        // Update the play button's disabled state to match the slot content.
        var rows = list.children;
        if (rows && rows[j]) {
          var btns = rows[j].querySelectorAll("button");
          if (btns.length >= 1) btns[0].disabled = !arr[j];
        }
      }
    }
  }

  // ─── TX buffer ─────────────────────────────────────────────────────────
  //
  // Renders three regions from /state: sent (locked, grey), in-flight
  // (locked, accent), pending (editable input). The pending input is
  // bound to buffer.slice(txEditableStart) — characters before that
  // index are immutable (the operator can't see them in the input).
  // The "dirty" pattern from bindMemory is reused so a poll during
  // mid-typing doesn't clobber the user's input.
  function renderTx(s){
    if (!s) return;
    var sent    = s.txSent    || 0;
    var head    = s.txHead    || 0;
    var len     = s.txLen     || 0;
    var active  = !!s.txActive;
    var buf     = s.txBuffer  || "";
    var cStart  = s.txChunkStart || 0;
    var cLen    = s.txChunkLen   || 0;

    var sentSpan  = $("txSent");
    var flightSpan= $("txFlight");
    var pending   = $("txPending");
    var startBtn  = $("txStart");
    var stopBtn   = $("txStop");
    var status    = $("txStatus");

    sentSpan.textContent  = buf.substring(0, sent);
    // In-flight region: chars from txSent to the end of the current
    // chunk (txChunkStart + txChunkLen). Empty when no chunk is playing.
    var flightEnd = (cLen > 0) ? (cStart + cLen) : sent;
    flightSpan.textContent = buf.substring(sent, flightEnd);

    // The pending input shows the editable tail (everything from
    // txEditableStart). Re-sync from /state only when the user isn't
    // mid-typing (the dirty flag protects in-progress edits).
    var editableStart = sent; // matches txEditableStart when cLen==0
    if (cLen > 0) editableStart = cStart + cLen;
    var expected = buf.substring(editableStart);
    if (pending.dataset.dirty !== "1" && pending.value !== expected) {
      pending.value = expected;
    }
    if (pending.dataset.dirty === "1" && pending.value === expected) {
      pending.dataset.dirty = "0";
    }

    // Buttons reflect state.
    startBtn.disabled = active || (sent >= len && len > 0) || len === 0;
    startBtn.hidden   = active;
    stopBtn.hidden    = !active;

    // Status text: short summary of progress.
    if (active) {
      status.textContent = "sending — " + sent + " / " + len + " chars";
    } else if (len === 0) {
      status.textContent = "idle";
    } else if (sent >= len) {
      status.textContent = "complete — " + len + " chars sent";
    } else {
      status.textContent = "armed — " + (len - sent) + " pending";
    }
  }

  function pollState(){
    fetch("/state", {cache:"no-store"}).then(function(r){
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    }).then(function(s){
      errors = 0;
      renderState(s);
    }).catch(function(){
      errors++;
      if (errors >= 3) showBanner("Disconnected — retrying…");
    });
  }

  function postJson(url, obj){
    return fetch(url, {method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify(obj)}).then(function(r){
      return r.json().then(function(j){return {ok:r.ok, status:r.status, body:j};});
    });
  }

  function bindSettings(){
    var ids = [
      ["wpm","wpm"], ["freq","frequency"], ["vol","volume"],
      ["polarity","polarityReversed"], ["keying","radioKeyingEnabled"],
      ["keyerType","keyerType"], ["winkey","winkeyMode"]
    ];
    ids.forEach(function(pair){
      var el = $(pair[0]);
      var key = pair[1];
      el.addEventListener("change", function(){
        var val;
        if (el.type === "checkbox") val = el.checked;
        else if (el.tagName === "SELECT") val = el.value;
        else val = parseFloat(el.value);
        if (key === "radioKeyingEnabled" && val === true){
          if (!confirm("Enable CW output on GPIO4?")) {
            patchChecked(el, last.radioKeyingEnabled);
            return;
          }
        }
        el.disabled = true;
        postJson("/api/settings", (function(){
          var o = {}; o[key] = val; return o;
        }())).then(function(res){
          el.disabled = false;
          if (!res.ok){
            showBanner("Save failed (HTTP " + res.status + ")");
            pollState();
          }
        }).catch(function(){
          el.disabled = false;
          showBanner("Save failed — device offline?");
        });
      });
    });
  }

  function bindMemory(){
    $("memList").addEventListener("click", function(ev){
      var t = ev.target;
      if (t.tagName !== "BUTTON") return;

      // Play button (▶) — fire-and-forget POST to /api/play.
      if (t.dataset.playSlot !== undefined) {
        var pslot = parseInt(t.dataset.playSlot, 10);
        if (isNaN(pslot)) return;
        var prev = t.textContent;
        t.disabled = true;
        t.textContent = "…";
        postJson("/api/play", {slot: pslot}).then(function(res){
          t.disabled = false;
          t.textContent = prev;
          if (!res.ok) showBanner("Play failed (HTTP " + res.status + ")");
        }).catch(function(){
          t.disabled = false;
          t.textContent = prev;
          showBanner("Play failed — device offline?");
        });
        return;
      }

      // Save button.
      var slot = parseInt(t.dataset.slot, 10);
      var input = $("mem-" + slot);
      if (!input) return;
      t.disabled = true;
      t.textContent = "Saving…";
      postJson("/api/memory", {slot: slot, text: input.value}).then(function(res){
        t.disabled = false;
        t.textContent = "Save";
        if (!res.ok) showBanner("Memory save failed (HTTP " + res.status + ")");
        else {
          // The server now matches the input — drop the dirty flag so
          // the next /state poll can sync this slot again.
          input.dataset.dirty = "0";
          pollState();
        }
      }).catch(function(){
        t.disabled = false;
        t.textContent = "Save";
        showBanner("Memory save failed — device offline?");
      });
    });
  }

  function bindTx(){
    var pending = $("txPending");
    if (!pending) return;

    // Keystroke-level edits. We POST one edit per character to keep
    // the server authoritative — the polled /state is reconciled with
    // the input on every tick (with the dirty flag guarding against
    // race-induced stomping).
    pending.addEventListener("input", function(ev){
      var el = ev.target;
      var newVal = el.value;
      var prevVal = el.dataset.prevValue || "";
      el.dataset.dirty = "1";
      if (newVal.length > prevVal.length) {
        // Insert — POST the appended char(s).
        for (var i = prevVal.length; i < newVal.length; i++){
          (function(c){
            postJson("/api/tx/edit", {op:"append", c:c}).then(function(res){
              if (!res.ok){
                el.dataset.dirty = "0";
                showBanner("TX edit: " + (res.body && res.body.error || ("HTTP " + res.status)));
                pollState();
              } else if (i === newVal.length - 1) {
                el.dataset.dirty = "0";
                pollState();
              }
            }).catch(function(){
              showBanner("TX edit failed — device offline?");
              el.dataset.dirty = "0";
              pollState();
            });
          })(newVal.charAt(i));
        }
      } else if (newVal.length < prevVal.length) {
        // Backspace — POST one backspace per removed char.
        var removed = prevVal.length - newVal.length;
        var posted = 0;
        function postBs(){
          postJson("/api/tx/edit", {op:"backspace"}).then(function(res){
            if (!res.ok){
              el.dataset.dirty = "0";
              showBanner("TX backspace: " + (res.body && res.body.error || ("HTTP " + res.status)));
              pollState();
              return;
            }
            posted++;
            if (posted >= removed){
              el.dataset.dirty = "0";
              pollState();
            } else {
              postBs();
            }
          }).catch(function(){
            showBanner("TX backspace failed — device offline?");
            el.dataset.dirty = "0";
            pollState();
          });
        }
        postBs();
      }
      el.dataset.prevValue = newVal;
    });

    // TX button — arm the session.
    $("txStart").addEventListener("click", function(){
      var btn = $("txStart");
      btn.disabled = true;
      postJson("/api/tx/start", {}).then(function(res){
        btn.disabled = false;
        if (!res.ok){
          showBanner("TX start failed: " + (res.body && res.body.error || ("HTTP " + res.status)));
        }
        pollState();
      }).catch(function(){
        btn.disabled = false;
        showBanner("TX start failed — device offline?");
      });
    });

    // Stop button — cancel in-flight.
    $("txStop").addEventListener("click", function(){
      postJson("/api/tx/stop", {}).then(function(res){
        if (!res.ok) showBanner("TX stop failed");
        pollState();
      }).catch(function(){
        showBanner("TX stop failed — device offline?");
      });
    });

    // Clear button — confirm, then wipe.
    $("txClear").addEventListener("click", function(){
      if (!confirm("Clear TX buffer?")) return;
      postJson("/api/tx/clear", {}).then(function(res){
        if (!res.ok) showBanner("TX clear failed");
        var pendingEl = $("txPending");
        if (pendingEl){
          pendingEl.value = "";
          pendingEl.dataset.dirty = "0";
          pendingEl.dataset.prevValue = "";
        }
        pollState();
      }).catch(function(){
        showBanner("TX clear failed — device offline?");
      });
    });
  }

  document.addEventListener("DOMContentLoaded", function(){
    bindSettings();
    bindMemory();
    bindTx();
    pollState();
    setInterval(pollState, 500);
  });
})();
</script>
</body>
</html>)rawliteral";

#else  // ENABLE_WIFI_DEBUG == 0  OR  UNIT_TEST

void WebUI::begin(uint16_t) {}
void WebUI::poll() {}

#endif
