#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "muuk/ctrader_fix/gemini_client.hpp"

using namespace muuk::ctrader_fix;

namespace {

// A scripted transport: answers from a queue, records every call. No
// network ever happens in these tests.
class FakeTransport final : public IGeminiTransport {
public:
    void queue(GeminiHttpResult r) { scripted_.push_back(std::move(r)); }

    [[nodiscard]] GeminiHttpResult post(const std::string& url,
                                        const std::vector<std::pair<std::string, std::string>>& headers,
                                        const std::string& body) override {
        ++calls_;
        lastUrl_ = url;
        lastHeaders_ = headers;
        lastBody_ = body;
        if (scripted_.empty()) {
            GeminiHttpResult r;
            r.networkError = true;
            return r;
        }
        GeminiHttpResult r = scripted_.front();
        scripted_.pop_front();
        return r;
    }

    int calls_ = 0;
    std::string lastUrl_;
    std::vector<std::pair<std::string, std::string>> lastHeaders_;
    std::string lastBody_;

private:
    std::deque<GeminiHttpResult> scripted_;
};

GeminiHttpResult httpResult(int status, const std::string& body, int retryAfterMs = 0) {
    GeminiHttpResult r;
    r.httpStatus = status;
    r.body = body;
    r.retryAfterMs = retryAfterMs;
    return r;
}

std::string happyBody(const std::string& text) {
    nlohmann::json j;
    j["candidates"] = nlohmann::json::array(
        {{{"content", {{"parts", nlohmann::json::array({{{"text", text}}})}}}, {"finishReason", "STOP"}}});
    return j.dump();
}

bool hasHeader(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name) {
    for (const auto& h : headers) {
        if (h.first == name) return true;
    }
    return false;
}

std::string headerValue(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name) {
    for (const auto& h : headers) {
        if (h.first == name) return h.second;
    }
    return std::string();
}

// A client whose transport is the fake and whose sleeper only records the
// waits (never actually sleeps).
struct ClientWithFake {
    std::shared_ptr<FakeTransport> fake = std::make_shared<FakeTransport>();
    std::vector<int> sleeps;
    GeminiClient client{"test-key-123", GeminiClient::kDefaultModel, GeminiClient::kFallbackModel, fake,
                        [this](int ms) { sleeps.push_back(ms); }};
};

}  // namespace

TEST_CASE("Gemini client: buildUrl puts the model setting in the path, never a key") {
    const std::string url = GeminiClient::buildUrl("gemini-3.5-flash-lite");
    REQUIRE(url == "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent");
    const std::string other = GeminiClient::buildUrl("gemini-3.8-flash");
    REQUIRE(other.find("gemini-3.8-flash") != std::string::npos);
    REQUIRE(other.find(":generateContent") != std::string::npos);
}

TEST_CASE("Gemini client: request body has the exact system_instruction + contents shape, no generationConfig") {
    const std::string body = GeminiClient::buildRequestBody("SYSTEM", "USER QUESTION");
    const nlohmann::json j = nlohmann::json::parse(body);
    REQUIRE(j["system_instruction"]["parts"][0]["text"].get<std::string>() == "SYSTEM");
    REQUIRE(j["contents"].size() == 1);
    REQUIRE(j["contents"][0]["role"].get<std::string>() == "user");
    REQUIRE(j["contents"][0]["parts"][0]["text"].get<std::string>() == "USER QUESTION");
    REQUIRE((!j.contains("temperature")));
    REQUIRE((!j.contains("generationConfig")));
    REQUIRE((!j.contains("safetySettings")));
    REQUIRE(body.find("test-key") == std::string::npos);
}

TEST_CASE("Gemini client: happy path sends POST-shaped request and returns the answer text") {
    ClientWithFake t;
    t.fake->queue(httpResult(200, happyBody("Brain ne cycle is liye nahi khola.")));
    const GeminiResponse res = t.client.ask("Aaj cycle kyun nahi khula?");
    REQUIRE(res.ok);
    REQUIRE(res.errorKind == GeminiErrorKind::None);
    REQUIRE(res.text == "Brain ne cycle is liye nahi khola.");
    REQUIRE(res.httpStatus == 200);
    REQUIRE(t.fake->calls_ == 1);
    REQUIRE(t.fake->lastUrl_.find("gemini-3.5-flash-lite") != std::string::npos);
    REQUIRE(hasHeader(t.fake->lastHeaders_, "x-goog-api-key"));
    REQUIRE(headerValue(t.fake->lastHeaders_, "x-goog-api-key") == "test-key-123");
    REQUIRE(t.fake->lastUrl_.find("test-key-123") == std::string::npos);
    const nlohmann::json sent = nlohmann::json::parse(t.fake->lastBody_);
    REQUIRE(sent["contents"][0]["parts"][0]["text"].get<std::string>() == "Aaj cycle kyun nahi khula?");
    REQUIRE(sent["system_instruction"]["parts"][0]["text"].get<std::string>() ==
            std::string(GeminiClient::kDefaultSystemPrompt));
}

TEST_CASE("Gemini client: response text joins all parts, not just the first") {
    nlohmann::json j;
    j["candidates"] = nlohmann::json::array({{{"content",
                                              {{"parts", nlohmann::json::array({{{"text", "pehla "}}, {{"text", "doosra"}}})}}},
                                             {"finishReason", "STOP"}}});
    const GeminiResponse res = GeminiClient::parseResponse(200, j.dump(), "gemini-3.5-flash-lite");
    REQUIRE(res.ok);
    REQUIRE(res.text == "pehla doosra");
}

TEST_CASE("Gemini client: promptFeedback.blockReason wins over candidates") {
    nlohmann::json j;
    j["promptFeedback"] = {{"blockReason", "SAFETY"}};
    j["candidates"] = nlohmann::json::array(
        {{{"content", {{"parts", nlohmann::json::array({{{"text", "must not be shown"}}})}}}, {"finishReason", "STOP"}}});
    const GeminiResponse res = GeminiClient::parseResponse(200, j.dump(), "gemini-3.5-flash-lite");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::Blocked);
    REQUIRE(res.text.empty());
    REQUIRE((!res.retryable));
    REQUIRE(res.error.find("SAFETY") != std::string::npos);
}

TEST_CASE("Gemini client: 400 API_KEY_INVALID maps to InvalidKey, not retryable") {
    const std::string body =
        R"({"error": {"code": 400, "message": "API key not valid. Please pass a valid API key.", "status": "API_KEY_INVALID"}})";
    const GeminiResponse res = GeminiClient::parseResponse(400, body, "gemini-3.5-flash-lite");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::InvalidKey);
    REQUIRE(res.httpStatus == 400);
    REQUIRE((!res.retryable));
    REQUIRE(res.error.find("Keys popup") != std::string::npos);
}

TEST_CASE("Gemini client: 429 RESOURCE_EXHAUSTED maps to RateLimited with the first backoff") {
    const std::string body =
        R"({"error": {"code": 429, "message": "Quota exceeded.", "status": "RESOURCE_EXHAUSTED"}})";
    const GeminiResponse res = GeminiClient::parseResponse(429, body, "gemini-3.5-flash-lite");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::RateLimited);
    REQUIRE(res.httpStatus == 429);
    REQUIRE(res.retryable);
    REQUIRE(res.retryAfterMs == 5'000);
    REQUIRE(res.error.find("aadhi raat") != std::string::npos);
}

TEST_CASE("Gemini client: malformed JSON on a 200 is a ParseError, never a crash or empty success") {
    const GeminiResponse res = GeminiClient::parseResponse(200, "{not json", "gemini-3.5-flash-lite");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::ParseError);
    REQUIRE(res.text.empty());
}

TEST_CASE("Gemini client: empty candidates is a ParseError") {
    const GeminiResponse res = GeminiClient::parseResponse(200, R"({"candidates": []})", "gemini-3.5-flash-lite");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::ParseError);
    REQUIRE(res.error.find("khaali jawab") != std::string::npos);
}

TEST_CASE("Gemini client: missing candidates and empty parts are ParseErrors") {
    const GeminiResponse missing = GeminiClient::parseResponse(200, R"({})", "gemini-3.5-flash-lite");
    REQUIRE((!missing.ok));
    REQUIRE(missing.errorKind == GeminiErrorKind::ParseError);
    nlohmann::json j;
    j["candidates"] = nlohmann::json::array({{{"content", {{"parts", nlohmann::json::array()}}}, {"finishReason", "STOP"}}});
    const GeminiResponse emptyParts = GeminiClient::parseResponse(200, j.dump(), "gemini-3.5-flash-lite");
    REQUIRE((!emptyParts.ok));
    REQUIRE(emptyParts.errorKind == GeminiErrorKind::ParseError);
}

TEST_CASE("Gemini client: non-STOP finishReason with no text is a ParseError, with text it is still an answer") {
    nlohmann::json j;
    j["candidates"] = nlohmann::json::array(
        {{{"content", {{"parts", nlohmann::json::array()}}}, {"finishReason", "MAX_TOKENS"}}});
    const GeminiResponse noText = GeminiClient::parseResponse(200, j.dump(), "gemini-3.5-flash-lite");
    REQUIRE((!noText.ok));
    REQUIRE(noText.errorKind == GeminiErrorKind::ParseError);
    REQUIRE(noText.error.find("MAX_TOKENS") != std::string::npos);

    nlohmann::json j2;
    j2["candidates"] = nlohmann::json::array({{{"content",
                                               {{"parts", nlohmann::json::array({{{"text", "aadha jawab"}}})}}},
                                              {"finishReason", "MAX_TOKENS"}}});
    const GeminiResponse withText = GeminiClient::parseResponse(200, j2.dump(), "gemini-3.5-flash-lite");
    REQUIRE(withText.ok);
    REQUIRE(withText.text == "aadha jawab");
}

TEST_CASE("Gemini client: 404 names the model from the setting") {
    const std::string body = R"({"error": {"code": 404, "message": "models/x is not found.", "status": "NOT_FOUND"}})";
    const GeminiResponse res = GeminiClient::parseResponse(404, body, "gemini-9.9-nope");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::HttpError);
    REQUIRE((!res.retryable));
    REQUIRE(res.error.find("gemini-9.9-nope") != std::string::npos);
}

TEST_CASE("Gemini client: 500 is a retryable HttpError") {
    const GeminiResponse res = GeminiClient::parseResponse(
        500, R"({"error": {"code": 500, "message": "Internal error.", "status": "INTERNAL"}})", "gemini-3.5-flash-lite");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::HttpError);
    REQUIRE(res.retryable);
    REQUIRE(res.retryAfterMs == 5'000);
}

TEST_CASE("Gemini client: 429 then success retries once, sleeping the first backoff") {
    ClientWithFake t;
    t.fake->queue(httpResult(429, R"({"error": {"code": 429, "message": "Quota exceeded.", "status": "RESOURCE_EXHAUSTED"}})"));
    t.fake->queue(httpResult(200, happyBody("dobara koshish par mil gaya.")));
    const GeminiResponse res = t.client.ask("sawal?");
    REQUIRE(res.ok);
    REQUIRE(res.text == "dobara koshish par mil gaya.");
    REQUIRE(t.fake->calls_ == 2);
    REQUIRE(t.sleeps.size() == 1);
    REQUIRE(t.sleeps[0] == 5'000);
}

TEST_CASE("Gemini client: server Retry-After beats the schedule on a retry") {
    ClientWithFake t;
    t.fake->queue(httpResult(
        429, R"({"error": {"code": 429, "message": "Quota exceeded.", "status": "RESOURCE_EXHAUSTED"}})", 12'000));
    t.fake->queue(httpResult(200, happyBody("theek hai.")));
    const GeminiResponse res = t.client.ask("sawal?");
    REQUIRE(res.ok);
    REQUIRE(t.sleeps.size() == 1);
    REQUIRE(t.sleeps[0] == 12'000);
}

TEST_CASE("Gemini client: persistent 429 stops after max 2 auto-retries with 5s then 15s waits") {
    ClientWithFake t;
    for (int i = 0; i < 5; ++i) {
        t.fake->queue(
            httpResult(429, R"({"error": {"code": 429, "message": "Quota exceeded.", "status": "RESOURCE_EXHAUSTED"}})"));
    }
    const GeminiResponse res = t.client.ask("sawal?");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::RateLimited);
    REQUIRE(t.fake->calls_ == 3);
    REQUIRE(t.sleeps.size() == 2);
    REQUIRE(t.sleeps[0] == 5'000);
    REQUIRE(t.sleeps[1] == 15'000);
    REQUIRE(res.retryAfterMs == 5'000);
}

TEST_CASE("Gemini client: network failure retries, then reports Network") {
    ClientWithFake t;
    GeminiHttpResult net;
    net.networkError = true;
    for (int i = 0; i < 3; ++i) t.fake->queue(net);
    const GeminiResponse res = t.client.ask("sawal?");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::Network);
    REQUIRE(res.retryable);
    REQUIRE(t.fake->calls_ == 3);
}

TEST_CASE("Gemini client: empty API key never reaches the transport") {
    ClientWithFake t;
    GeminiClient noKey("", GeminiClient::kDefaultModel, GeminiClient::kFallbackModel, t.fake,
                       [](int) {});
    const GeminiResponse res = noKey.ask("sawal?");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::InvalidKey);
    REQUIRE(t.fake->calls_ == 0);
}

TEST_CASE("Gemini client: backoff schedule is 5s, 15s, 45s") {
    REQUIRE(GeminiClient::backoffMs(0) == 5'000);
    REQUIRE(GeminiClient::backoffMs(1) == 15'000);
    REQUIRE(GeminiClient::backoffMs(2) == 45'000);
    REQUIRE(GeminiClient::backoffMs(9) == 45'000);
}

TEST_CASE("Gemini client: model is a setting, with the doc defaults") {
    REQUIRE(std::string(GeminiClient::kDefaultModel) == "gemini-3.5-flash-lite");
    REQUIRE(std::string(GeminiClient::kFallbackModel) == "gemini-3.8-flash");
    GeminiClient client("k", "gemini-3.8-flash");
    REQUIRE(client.model() == "gemini-3.8-flash");
    REQUIRE(client.fallbackModel() == "gemini-3.8-flash");
    GeminiClient defaulted("k", "");
    REQUIRE(defaulted.model() == "gemini-3.5-flash-lite");
}

#ifndef _WIN32
TEST_CASE("Gemini client: the non-Windows default transport returns NotSupported, never a fake success") {
    GeminiClient client("test-key-123");
    const GeminiResponse res = client.ask("sawal?");
    REQUIRE((!res.ok));
    REQUIRE(res.errorKind == GeminiErrorKind::NotSupported);
    REQUIRE(res.text.empty());
    REQUIRE((!res.retryable));
    REQUIRE(res.httpStatus == 0);
}
#endif
