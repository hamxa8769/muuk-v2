// GeminiClient: minimal Gemini generateContent POST client (v0.4.0 design
// Section 4.2-4.4). See gemini_client.hpp for the contract, the privacy
// rules, and why this shares no code with the GET-only updater.
//
// No logging happens anywhere in this file: the API key must never appear
// in a log, and full request/response bodies are never logged either.
// Callers log only status codes and error kinds.

#include "muuk/ctrader_fix/gemini_client.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cwchar>  // std::wcstol (Retry-After parsing)
#include <windows.h>
#include <winhttp.h>
#endif

namespace muuk::ctrader_fix {

namespace {

// Backoff schedule, design Section 4.3: 5s -> 15s -> 45s, then stop.
constexpr int kBackoffMs[] = {5'000, 15'000, 45'000};
// The auto-retry sleep is capped at the last schedule step even if the
// server asks for longer (free-tier RPD resets at midnight Pacific, so a
// huge Retry-After means "try again later by hand", not "sleep for hours").
constexpr int kMaxAutoSleepMs = 45'000;
constexpr std::size_t kMaxBodyBytes = 4 * 1024 * 1024;

// The stub transport used where no WinHTTP exists. It never fakes a
// success: every call reports notSupported.
class NotSupportedTransport final : public IGeminiTransport {
public:
    [[nodiscard]] GeminiHttpResult post(const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                                        const std::string&) override {
        GeminiHttpResult r;
        r.notSupported = true;
        r.networkErrorText = "Gemini POST client is only built on Windows";
        return r;
    }
};

#ifdef _WIN32
// Model names are plain ASCII, so a plain widening conversion is enough.
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

class WinHttpGeminiTransport final : public IGeminiTransport {
public:
    [[nodiscard]] GeminiHttpResult post(const std::string& url,
                                        const std::vector<std::pair<std::string, std::string>>& headers,
                                        const std::string& body) override {
        GeminiHttpResult result;
        // This transport only ever talks to the Gemini endpoint; anything
        // else is a programming error, not a network failure to retry.
        static const std::string kPrefix = "https://generativelanguage.googleapis.com";
        if (url.rfind(kPrefix, 0) != 0) {
            result.networkError = true;
            result.networkErrorText = "unexpected Gemini URL";
            return result;
        }
        const std::wstring path = widen(url.substr(kPrefix.size()));

        HINTERNET session = WinHttpOpen(L"Muuk-Gemini", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            // Older Windows (before 8.1) has no automatic proxy.
            session = WinHttpOpen(L"Muuk-Gemini", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (!session) {
            result.networkError = true;
            result.networkErrorText = "WinHTTP unavailable";
            return result;
        }
        // 30 s total per call (design Section 4.2).
        WinHttpSetTimeouts(session, 10'000, 10'000, 30'000, 30'000);

        HINTERNET connect = WinHttpConnect(session, L"generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                    : nullptr;
        if (!request) {
            if (connect) WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            result.networkError = true;
            result.networkErrorText = "could not start the request";
            return result;
        }

        std::wstring headersW;
        for (const auto& h : headers) headersW += widen(h.first) + L": " + widen(h.second) + L"\r\n";

        const bool sent =
            WinHttpSendRequest(request, headersW.c_str(), static_cast<DWORD>(headersW.size()), (LPVOID)body.data(),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) != 0;
        const bool received = sent && WinHttpReceiveResponse(request, nullptr) != 0;
        if (!received) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            result.networkError = true;
            result.networkErrorText = "could not reach Gemini";
            return result;
        }

        DWORD code = 0;
        DWORD size = sizeof(code);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            result.networkError = true;
            result.networkErrorText = "could not read the response status";
            return result;
        }
        result.httpStatus = static_cast<int>(code);

        // Retry-After is seconds when numeric; an HTTP-date is ignored (the
        // backoff schedule applies instead).
        wchar_t retryAfterBuf[64] = {};
        DWORD retryAfterSize = sizeof(retryAfterBuf);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, retryAfterBuf,
                                &retryAfterSize, WINHTTP_NO_HEADER_INDEX)) {
            const long seconds = std::wcstol(retryAfterBuf, nullptr, 10);
            if (seconds > 0) result.retryAfterMs = static_cast<int>(seconds) * 1'000;
        }

        std::string bodyOut;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            bodyOut.append(chunk.data(), read);
            if (bodyOut.size() > kMaxBodyBytes) break;
        }
        result.body = std::move(bodyOut);

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }
};
#endif  // _WIN32

}  // namespace

GeminiClient::GeminiClient(std::string apiKey, std::string model, std::string fallbackModel,
                           std::shared_ptr<IGeminiTransport> transport, std::function<void(int)> sleeper)
    : apiKey_(std::move(apiKey)),
      model_(model.empty() ? kDefaultModel : std::move(model)),
      fallbackModel_(fallbackModel.empty() ? kFallbackModel : std::move(fallbackModel)),
      transport_(transport ? std::move(transport) : makeDefaultTransport()),
      sleeper_(sleeper ? std::move(sleeper) : [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }) {
}

std::string GeminiClient::buildUrl(const std::string& model) {
    return "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
}

std::string GeminiClient::buildRequestBody(const std::string& systemPrompt, const std::string& userPrompt) {
    // Exact shape from the design doc (Section 4.2). No temperature /
    // generationConfig (unsupported on 3.6+ models), no safetySettings
    // override, and no key anywhere in the body.
    nlohmann::json body;
    body["system_instruction"]["parts"] = nlohmann::json::array({{{"text", systemPrompt}}});
    body["contents"] = nlohmann::json::array({{{"role", "user"}, {"parts", nlohmann::json::array({{{"text", userPrompt}}})}}});
    return body.dump();
}

int GeminiClient::backoffMs(int attempt) {
    if (attempt <= 0) return kBackoffMs[0];
    if (attempt == 1) return kBackoffMs[1];
    return kBackoffMs[2];
}

std::shared_ptr<IGeminiTransport> GeminiClient::makeDefaultTransport() {
#ifdef _WIN32
    return std::make_shared<WinHttpGeminiTransport>();
#else
    return std::make_shared<NotSupportedTransport>();
#endif
}

GeminiResponse GeminiClient::parseResponse(int httpStatus, const std::string& body, const std::string& model) {
    GeminiResponse res;
    res.httpStatus = httpStatus;

    // Never crash on a malformed body: parse without exceptions.
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        if (httpStatus == 200) {
            res.errorKind = GeminiErrorKind::ParseError;
            res.error = "Gemini se ghalat jawab mila — dobara koshish karein.";
        } else {
            res.errorKind = GeminiErrorKind::HttpError;
            res.error = "Gemini ne HTTP " + std::to_string(httpStatus) + " diya — thodi der baad dobara koshish karein.";
            res.retryable = (httpStatus == 429) || (httpStatus >= 500);
            if (res.retryable) res.retryAfterMs = backoffMs(0);
        }
        return res;
    }

    if (httpStatus != 200) {
        // Error object first: {"error": {"code": 400, "message": "...",
        // "status": "API_KEY_INVALID"}}.
        std::string status;
        std::string message;
        if (j.contains("error") && j["error"].is_object()) {
            status = j["error"].value("status", std::string());
            message = j["error"].value("message", std::string());
        }
        if (status == "API_KEY_INVALID") {
            // Invalid key is 400, NOT 401 (verified, design Section 4.3).
            res.errorKind = GeminiErrorKind::InvalidKey;
            res.error =
                "Gemini key ghalat hai ya expire ho gayi. Keys popup mein nayi key paste karein (Google AI Studio se).";
            return res;
        }
        if (httpStatus == 429 || status == "RESOURCE_EXHAUSTED") {
            // Free-tier RPM/TPM/RPD numbers are not published per-model, so
            // no limit is hard-coded: 429 is handled with backoff only.
            // RPD resets at midnight Pacific (design Section 4.3).
            res.errorKind = GeminiErrorKind::RateLimited;
            res.error =
                "Gemini ki limit khatam ho gayi (free tier). Thodi der baad dobara koshish karein — limit aadhi raat "
                "Pacific time par reset hoti hai.";
            res.retryable = true;
            res.retryAfterMs = backoffMs(0);
            return res;
        }
        if (httpStatus == 404) {
            res.errorKind = GeminiErrorKind::HttpError;
            res.error = "Model '" + model +
                        "' nahi mila — model setting check karein (default: gemini-3.5-flash-lite).";
            return res;
        }
        res.errorKind = GeminiErrorKind::HttpError;
        if (httpStatus >= 500) {
            res.error = "Gemini ka server busy hai — thodi der baad dobara koshish karein.";
            res.retryable = true;
            res.retryAfterMs = backoffMs(0);
        } else if (!message.empty()) {
            res.error = message;
        } else {
            res.error = "Gemini ne HTTP " + std::to_string(httpStatus) + " diya — thodi der baad dobara koshish karein.";
        }
        return res;
    }

    // 200: promptFeedback.blockReason FIRST (design Section 4.2) -- a
    // blocked prompt has no answer to show, whatever else is present.
    if (j.contains("promptFeedback") && j["promptFeedback"].is_object()) {
        const std::string blockReason = j["promptFeedback"].value("blockReason", std::string());
        if (!blockReason.empty()) {
            res.errorKind = GeminiErrorKind::Blocked;
            res.error = "Gemini ne is prompt ko block kar diya (reason: " + blockReason +
                        "). Sawal badal kar dobara poochhein.";
            return res;
        }
    }

    // candidates[0].content.parts[].text -- all parts joined, not just the
    // first (a reply may arrive in several parts).
    std::string finishReason;
    std::string text;
    if (j.contains("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
        const nlohmann::json& candidate = j["candidates"][0];
        if (candidate.is_object()) {
            finishReason = candidate.value("finishReason", std::string());
            if (candidate.contains("content") && candidate["content"].is_object() &&
                candidate["content"].contains("parts") && candidate["content"]["parts"].is_array()) {
                for (const auto& part : candidate["content"]["parts"]) {
                    if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                        text += part["text"].get<std::string>();
                    }
                }
            }
        }
    }
    if (text.empty()) {
        res.errorKind = GeminiErrorKind::ParseError;
        if (!finishReason.empty() && finishReason != "STOP") {
            res.error = "Gemini ne jawab mukammal nahi kiya (finishReason: " + finishReason +
                        ") — dobara koshish karein.";
        } else {
            res.error = "Gemini ne khaali jawab diya — dobara koshish karein.";
        }
        return res;
    }

    // Text present with a non-STOP finishReason is still a usable (partial)
    // answer; only an empty answer is an error.
    res.ok = true;
    res.text = std::move(text);
    res.errorKind = GeminiErrorKind::None;
    return res;
}

GeminiResponse GeminiClient::ask(const std::string& userPrompt, const std::string& systemPrompt) {
    GeminiRequest req;
    req.model = model_;
    req.systemPrompt = systemPrompt.empty() ? kDefaultSystemPrompt : systemPrompt;
    req.userPrompt = userPrompt;
    return ask(req);
}

GeminiResponse GeminiClient::ask(const GeminiRequest& req) {
    const std::string model = req.model.empty() ? model_ : req.model;
    const std::string systemPrompt = req.systemPrompt.empty() ? kDefaultSystemPrompt : req.systemPrompt;

    if (apiKey_.empty()) {
        GeminiResponse res;
        res.errorKind = GeminiErrorKind::InvalidKey;
        res.error = "Gemini key nahi hai — Keys popup mein key daalein.";
        return res;
    }

    GeminiResponse last;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        int serverWaitMs = 0;
        last = askOnce(model, systemPrompt, req.userPrompt, &serverWaitMs);
        if (last.ok || !last.retryable) return last;
        // Retryable failure: how long to wait before the next try -- the
        // server's Retry-After when present, else this attempt's backoff
        // schedule step (5s, then 15s). parseResponse's retryAfterMs is only
        // the first backoff step, so it must NOT be used here.
        const int waitMs = serverWaitMs > 0 ? serverWaitMs : backoffMs(attempt);
        if (attempt + 1 >= kMaxAttempts) {
            // Auto-retries exhausted: the caller retries by hand, starting a
            // fresh backoff sequence (server Retry-After wins when present).
            last.retryAfterMs = serverWaitMs > 0 ? serverWaitMs : backoffMs(0);
            return last;
        }
        last.retryAfterMs = waitMs;
        sleeper_(std::min(waitMs, kMaxAutoSleepMs));
    }
    return last;
}

GeminiResponse GeminiClient::askOnce(const std::string& model, const std::string& systemPrompt,
                                     const std::string& userPrompt, int* serverRetryAfterMs) {
    const std::string url = buildUrl(model);
    const std::string body = buildRequestBody(systemPrompt, userPrompt);
    // Auth: x-goog-api-key header only -- never in the URL, never logged.
    const std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json"},
                                                                      {"x-goog-api-key", apiKey_}};
    const GeminiHttpResult http = transport_->post(url, headers, body);
    *serverRetryAfterMs = http.retryAfterMs;

    if (http.notSupported) {
        GeminiResponse res;
        res.errorKind = GeminiErrorKind::NotSupported;
        res.error = "Gemini assistant is platform par supported nahi hai (Windows par hi chalta hai).";
        return res;
    }
    if (http.networkError) {
        GeminiResponse res;
        res.errorKind = GeminiErrorKind::Network;
        res.error = "Gemini se connect nahi ho saka — internet check karein aur dobara koshish karein.";
        res.retryable = true;
        res.retryAfterMs = backoffMs(0);
        return res;
    }
    GeminiResponse res = parseResponse(http.httpStatus, http.body, model);
    if (res.retryable && http.retryAfterMs > 0) res.retryAfterMs = http.retryAfterMs;
    return res;
}

}  // namespace muuk::ctrader_fix
