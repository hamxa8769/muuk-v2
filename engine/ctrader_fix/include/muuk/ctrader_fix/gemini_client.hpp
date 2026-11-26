#pragma once

// GeminiClient: the in-app AI assistant's POST client (Muuk v0.4.0 design,
// Section 4.2-4.4). A minimal WinHTTP POST client for the Gemini
// generateContent endpoint, deliberately separate from the updater path:
// the updater (app/desktop/src/updater.cpp) is GET-only and must never
// learn POST, so this client shares no code with it -- only the WinHTTP
// call pattern (WinHttpOpen / WinHttpConnect / WinHttpOpenRequest with
// "POST" / WinHttpSendRequest / WinHttpReceiveResponse / WinHttpReadData).
//
// generateContent is LEGACY but still supported. Model is a setting, never
// hard-coded: the constructor takes the model (default
// "gemini-3.5-flash-lite", fallback "gemini-3.8-flash"). Auth goes in the
// x-goog-api-key header -- never in the URL, never in a log. The request
// body is system_instruction + contents only (snake_case JSON); no
// temperature / generationConfig (unsupported on 3.6+ models) and no
// safetySettings override.
//
// Portability: the WinHTTP transport only exists on Windows
// (#ifdef _WIN32). On other platforms the default transport is a stub
// that returns NotSupported -- never a fake success. Tests inject a fake
// transport, so the client logic (body building, response parsing, error
// mapping, backoff) is fully testable on Linux with no network.
//
// Privacy (Section 4.4): the client accepts ONLY the caller-supplied
// prompt text and the configured system prompt. It has no fields for --
// and never forwards -- API keys other than its own, account numbers,
// passwords, balances, equity, margin, or file dumps. The API key is
// held in memory only, never written to any file, and never logged: this
// file performs no logging at all, and callers must log only status codes
// and error kinds. Free-tier prompts may be used by Google to improve
// its products, so callers must only send the allowlisted context
// (buildAssistantContext in the dashboard); the client cannot verify
// prompt contents, it can only refuse to carry anything else.
//
// Role flag (Section 4.6): the doc puts AssistantRole in LiveConfig, not
// in this client. There is intentionally no role parameter here; the
// dashboard enforces owner vs non-owner (AssistantAction::allowed) in
// B3, before any write is dispatched.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace muuk::ctrader_fix {

// Why a Gemini call failed. HttpError covers 4xx/5xx that are not one of
// the named cases (e.g. 404 model-not-found, 5xx server errors).
enum class GeminiErrorKind {
    None,         // success
    InvalidKey,   // 400 API_KEY_INVALID
    RateLimited,  // 429 RESOURCE_EXHAUSTED
    Blocked,      // promptFeedback.blockReason present
    Network,      // WinHTTP failure / timeout
    HttpError,    // any other non-200 status
    ParseError,   // malformed JSON, missing/empty candidates, empty text
    NotSupported  // non-Windows default transport (Windows-only client)
};

// One generateContent call, as the design doc (Section 4.2) specifies it.
// `model` is a setting: empty means "use the client's configured model".
struct GeminiRequest {
    std::string model;         // e.g. "gemini-3.5-flash-lite" -- a setting, never hard-coded
    std::string systemPrompt;  // assistant role + rules (Section 4.5)
    std::string userPrompt;    // user question + allowlisted context only (Section 4.4)
};

// The outcome of one ask(). `error` is one user-facing line (Section 4.3);
// `retryAfterMs` is how long the caller should wait before retrying a
// retryable failure (server Retry-After when present, else the backoff
// schedule); 0 when no wait applies.
struct GeminiResponse {
    bool ok = false;
    std::string text;  // candidates[0].content.parts[].text, all parts joined
    std::string error;  // user-facing one-liner when !ok
    GeminiErrorKind errorKind = GeminiErrorKind::None;
    int httpStatus = 0;  // 200 / 400 / 429 / ... ; 0 when no HTTP round-trip happened
    bool retryable = false;  // 429 / 5xx / network -- retry with backoff
    int retryAfterMs = 0;    // ms to wait before a retry (0 = no wait)
};

// Raw result of one HTTP round-trip, as the transport reports it.
struct GeminiHttpResult {
    int httpStatus = 0;              // 0 when no HTTP response was received
    std::string body;                // response body (may be empty / non-JSON)
    int retryAfterMs = 0;            // Retry-After header, in ms (0 = header absent)
    bool networkError = false;       // transport failure (no HTTP response)
    bool notSupported = false;       // platform stub: no transport exists here
    std::string networkErrorText;    // short transport failure detail (never contains the key)
};

// Transport seam (Section 4.2 tests use this; no live network in tests).
// The production transport is WinHTTP on Windows; the default transport on
// any other platform is a stub returning notSupported = true.
class IGeminiTransport {
public:
    virtual ~IGeminiTransport() = default;
    // POST url with the given headers ("Name: value" pairs are passed as
    // {name, value}) and body. Never throws.
    [[nodiscard]] virtual GeminiHttpResult post(
        const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body) = 0;
};

class GeminiClient {
public:
    // Model defaults from the design doc (Section 4.2). The fallback is a
    // setting the caller may use after a 404 -- the client never silently
    // switches models on its own (Section 4.3: 404 is an error).
    static constexpr const char* kDefaultModel = "gemini-3.5-flash-lite";
    static constexpr const char* kFallbackModel = "gemini-3.8-flash";
    static constexpr int kTimeoutMs = 30'000;  // per call, design Section 4.2
    static constexpr int kMaxAttempts = 3;     // initial try + max 2 auto-retries (Section 4.3)
    // System prompt, exact wording from the design doc (Section 4.5).
    static constexpr const char* kDefaultSystemPrompt = R"(You are the in-app assistant for Muuk, a demo-only XAUUSD range trading bot.
You explain what the bot and its brain are doing, using ONLY the context given
in the user message. You never invent numbers, prices or trades.
Rules:
- Answer in Roman Urdu (the user's language). Keep it short and concrete.
- If the context does not contain the answer, say so — do not guess.
- Never reveal or ask for API keys, passwords, account numbers or balances.
- Never suggest removing or loosening a risk cap, a gate, or the $150 lock cap.
- Never claim a trade will profit or that the market will move a certain way.
- If the user asks to change a setting, list the exact change (field, old value,
  new value) and wait for confirmation — never apply it silently.)";

    // apiKey is held in memory only: never logged, never written to a file.
    // `transport` defaults to the platform transport (WinHTTP on Windows,
    // NotSupported stub elsewhere); tests inject a fake. `sleeper` defaults
    // to a real sleep between auto-retries; tests inject a recorder/no-op.
    // An empty model falls back to kDefaultModel; an empty fallbackModel
    // falls back to kFallbackModel.
    GeminiClient(std::string apiKey, std::string model = kDefaultModel, std::string fallbackModel = kFallbackModel,
                 std::shared_ptr<IGeminiTransport> transport = nullptr,
                 std::function<void(int)> sleeper = nullptr);

    [[nodiscard]] const std::string& model() const { return model_; }
    [[nodiscard]] const std::string& fallbackModel() const { return fallbackModel_; }

    // Synchronous POST on the caller's thread (the dashboard runs this on a
    // background thread -- the UI thread must never block). Returns the
    // parsed text or a typed error. Never throws.
    [[nodiscard]] GeminiResponse ask(const std::string& userPrompt,
                                     const std::string& systemPrompt = kDefaultSystemPrompt);
    [[nodiscard]] GeminiResponse ask(const GeminiRequest& req);

    // Pure helpers, exposed for tests (no network, no key involved).
    [[nodiscard]] static std::string buildUrl(const std::string& model);
    [[nodiscard]] static std::string buildRequestBody(const std::string& systemPrompt,
                                                      const std::string& userPrompt);
    // Parses one HTTP response into a GeminiResponse. Parse order is the
    // doc's (Section 4.2): error object for non-200, then
    // promptFeedback.blockReason, then candidates[0].content.parts[].text
    // (all parts joined), then empty/missing candidates and non-STOP
    // finishReason with no text. `model` is only used for the 404 message.
    [[nodiscard]] static GeminiResponse parseResponse(int httpStatus, const std::string& body,
                                                      const std::string& model);
    // Backoff schedule (Section 4.3): 5s -> 15s -> 45s, then stop.
    // attempt 0 = wait before the 1st retry, 1 = before the 2nd retry.
    [[nodiscard]] static int backoffMs(int attempt);

    // The platform default transport: WinHTTP on Windows, the NotSupported
    // stub on any other platform.
    [[nodiscard]] static std::shared_ptr<IGeminiTransport> makeDefaultTransport();

private:
    // One attempt. `serverRetryAfterMs` (out, never null) receives the
    // transport's Retry-After header in ms, or 0 when the server sent none.
    // It is separate from GeminiResponse::retryAfterMs, which parseResponse
    // pre-fills with the first backoff step -- using that for the sleep
    // would pin every retry to 5s instead of advancing the schedule.
    [[nodiscard]] GeminiResponse askOnce(const std::string& model, const std::string& systemPrompt,
                                         const std::string& userPrompt, int* serverRetryAfterMs);

    std::string apiKey_;  // memory only -- never logged, never written to a file
    std::string model_;
    std::string fallbackModel_;
    std::shared_ptr<IGeminiTransport> transport_;
    std::function<void(int)> sleeper_;
};

}  // namespace muuk::ctrader_fix
