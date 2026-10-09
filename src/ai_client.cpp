#include "ai_client.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

constexpr int kProtocol = 1;
constexpr size_t kMaxResponseBytes = 20'000'000;  // large enough for a voice WAV
constexpr size_t kMaxReplyBytes = 4000;  // 1000 Unicode characters, up to 4 UTF-8 bytes each

std::wstring toWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Closes a WinHTTP handle when it goes out of scope.
struct HttpHandle {
    HINTERNET handle = nullptr;
    ~HttpHandle()
    {
        if (handle) {
            WinHttpCloseHandle(handle);
        }
    }
};

// Reads a string field, or "" if it is missing or not a string.
std::string stringField(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

// Turns a failed reply into a readable message such as "HTTP 401 unauthorized: ...".
std::string describeFailure(int status, const std::string& body)
{
    json reply = json::parse(body, nullptr, false);
    std::string message = "HTTP " + std::to_string(status);
    if (reply.is_object() && reply.contains("error") && reply["error"].is_object()) {
        message += " " + stringField(reply["error"], "code") + ": " + stringField(reply["error"], "message");
    }
    return message;
}

// A JSON request body. "replace" turns invalid UTF-8 from the game into U+FFFD instead of throwing.
std::string jsonBody(const json& request)
{
    return request.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Ids and action names use the protocol's character sets and lengths.
bool isSafe(const std::string& text, size_t minLength, size_t maxLength, bool allowIdPunctuation)
{
    if (text.size() < minLength || text.size() > maxLength) {
        return false;
    }
    for (char c : text) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
            || (allowIdPunctuation && (c == '.' || c == ':' || c == '-'));
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Checks one returned action. Only "name" and the optional "args" are accepted, and the only
// argument is target_id, which must be the NPC this request was sent to.
TurnAction readAction(const json& action, const std::string& npcId)
{
    TurnAction result;
    if (!action.is_object()) {
        result.problem = "malformed action";
        return result;
    }
    result.name = stringField(action, "name");
    if (!isSafe(result.name, 1, 40, false)) {
        result.name.clear();
        result.problem = "malformed action name";
        return result;
    }
    for (auto it = action.begin(); it != action.end(); ++it) {
        if (it.key() != "name" && it.key() != "args") {
            result.problem = "unknown action field";
            return result;
        }
    }
    auto args = action.find("args");
    if (args == action.end()) {
        return result;  // name-only, as older servers send it
    }
    if (!args->is_object() || args->size() != 1 || !args->contains("target_id")) {
        result.problem = "unsupported action arguments";
        return result;
    }
    result.targetId = stringField(*args, "target_id");
    if (!isSafe(result.targetId, 1, 64, true) || result.targetId != npcId) {
        result.problem = "action target is not the NPC this request was sent to";
    }
    return result;
}

// A WAV file starts with "RIFF", a 4-byte size, then "WAVE".
bool isWav(const std::string& bytes)
{
    return bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WAVE") == 0;
}

}  // namespace

std::string newRequestId(const char* prefix)
{
    std::random_device random;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.8s-%08x%08x%08x", prefix, random(), random(), random());
    return buffer;
}

AiClient::Response AiClient::send(const wchar_t* method, const std::string& path, const std::string& contentType,
                                  const std::string& body, const std::string& extraHeaders) const
{
    Response response;
    // WinHTTP's timers below are per phase and coarse, so a slow server can keep this call
    // blocked for longer than timeout_seconds. This deadline does not shorten that wait; it
    // makes sure a reply that arrives after the budget is thrown away instead of used.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config_.timeoutSeconds);
    auto discardIfLate = [&]() {
        if (std::chrono::steady_clock::now() <= deadline) {
            return false;
        }
        response.status = 0;
        response.body.clear();
        response.error = "server reply took longer than timeout_seconds; the late reply was discarded";
        return true;
    };
    std::wstring url = toWide(config_.serverUrl + path);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        response.error = "server_url is not a valid http(s) URL";
        return response;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);

    int timeoutMs = config_.timeoutSeconds * 1000;
    HttpHandle session{WinHttpOpen(L"DwemerAiExample/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle) {
        response.error = "WinHTTP could not start";
        return response;
    }
    HttpHandle connection{WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0)};
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request{connection.handle ? WinHttpOpenRequest(connection.handle, method, target.c_str(), nullptr,
                                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                                         : nullptr};
    // timeout_seconds limits each phase on this request: name lookup, connect, send, waiting for
    // the reply headers (WinHTTP's own default for that wait is 90 seconds, so it is set
    // explicitly) and each body read. It is not one deadline for the whole request: a server
    // that keeps sending slowly can take longer in total (the deadline above then discards the
    // reply). WinHTTP checks these timers coarsely (a 1 second setting fired after about 2-4
    // seconds in tests).
    DWORD waitMs = static_cast<DWORD>(timeoutMs);
    bool timeoutsSet = request.handle && WinHttpSetTimeouts(request.handle, timeoutMs, timeoutMs, timeoutMs, timeoutMs)
        && WinHttpSetOption(request.handle, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &waitMs, sizeof(waitMs));
    // Never forward the bearer token to a redirect destination.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!timeoutsSet || !WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY,
                                          &redirectPolicy, sizeof(redirectPolicy))) {
        response.error = "cannot configure HTTP request (WinHTTP error " + std::to_string(GetLastError()) + ")";
        return response;
    }

    std::wstring headers = L"Authorization: Bearer " + toWide(config_.token) + L"\r\n";
    if (!contentType.empty()) {
        headers += L"Content-Type: " + toWide(contentType) + L"\r\n";
    }
    headers += toWide(extraHeaders);
    void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    DWORD size = static_cast<DWORD>(body.size());
    bool sent = request.handle
        && WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(-1), data, size, size, 0)
        && WinHttpReceiveResponse(request.handle, nullptr);
    if (!sent) {
        DWORD code = GetLastError();
        if (code == ERROR_WINHTTP_TIMEOUT) {
            response.error = "no answer from " + config_.serverUrl + " within timeout_seconds (WinHTTP error 12002)";
            return response;
        }
        response.error = "cannot reach " + config_.serverUrl + " (WinHTTP error " + std::to_string(code) + ")";
        return response;
    }

    if (discardIfLate()) {
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        response.error = "cannot read HTTP status";
        return response;
    }
    response.status = static_cast<int>(status);

    // Read only what has arrived, so the deadline is checked between chunks of a slow body.
    char buffer[8192];
    DWORD available = 0;
    DWORD read = 0;
    while (true) {
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            response.error = "HTTP response read failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
            return response;
        }
        if (available == 0) {
            break;
        }
        DWORD wanted = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
        if (!WinHttpReadData(request.handle, buffer, wanted, &read)) {
            response.error = "HTTP response read failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
            return response;
        }
        if (read == 0) {
            break;
        }
        response.body.append(buffer, read);
        if (response.body.size() > kMaxResponseBytes) {
            response.error = "server reply is too large";
            return response;
        }
        if (discardIfLate()) {
            return response;
        }
    }
    discardIfLate();
    return response;
}

std::string AiClient::Response::failure() const
{
    if (!error.empty()) {
        return error;
    }
    if (status != 200) {
        return describeFailure(status, body);
    }
    return "";
}

bool isValidId(const std::string& id)
{
    return isSafe(id, 1, 64, true);
}

bool isEventType(const std::string& type)
{
    return type == "location_entered" || type == "item_given" || type == "combat_started" || type == "combat_ended";
}

std::string AiClient::health(std::string* note, std::string* info) const
{
    Response response = send(L"GET", "/health.php", "", "");
    if (std::string failure = response.failure(); !failure.empty()) {
        return failure;
    }
    json reply = json::parse(response.body, nullptr, false);
    if (!reply.is_object() || reply["ok"] != true) {
        return "unexpected health reply (is server_url the ExampleServer folder?)";
    }
    if (reply["protocol"] != kProtocol) {
        return "server speaks protocol " + reply["protocol"].dump() + ", this client needs " + std::to_string(kProtocol)
            + ". Update the older side.";
    }
    // Optional list; servers before it still work with name-only actions.
    bool targets = false;
    if (reply["capabilities"].is_array()) {
        for (const json& item : reply["capabilities"]) {
            targets = targets || item == "action_target";
        }
    }
    if (note && !targets) {
        *note = "older server: no action targets or game events; name-only actions still work.";
    }
    // Optional fields: older servers send no server_version or baseline (or no expected_schema).
    // Shown only if they are short safe ids.
    if (info) {
        std::string version = stringField(reply, "server_version");
        std::string baseline = stringField(reply, "baseline");
        std::string schema = stringField(reply, "schema");
        *info = "server " + (isValidId(version) ? version : std::string("version not reported (older server)"))
            + ", server baseline " + (isValidId(baseline) ? baseline : std::string("not reported (older server)"))
            + ", schema " + (isValidId(schema) ? schema : std::string("not reported"));
    }
    return "";
}

TurnResult AiClient::sendTurn(const std::string& requestId, const GameContext& context, const std::string& text) const
{
    TurnResult result;
    result.requestId = requestId;

    json request = {
        {"protocol", kProtocol},
        {"request_id", requestId},
        {"session_id", config_.sessionId},
        {"npc", {{"id", context.npcId}, {"name", context.npcName}}},
        {"player", {{"name", context.playerName}}},
        {"context", context.details},
        {"text", text},
    };
    Response response = send(L"POST", "/turn.php", "application/json", jsonBody(request));
    readTurnReply(response, context, result);
    return result;
}

TurnResult AiClient::sendEvent(const std::string& requestId, const GameContext& context, const std::string& type,
                               const std::string& text, bool respond) const
{
    TurnResult result;
    result.requestId = requestId;
    result.logOnly = !respond;
    // Checked here too, so a bad event never leaves the game. Counts UTF-8 characters.
    size_t characters = 0;
    for (unsigned char c : text) {
        characters += (c & 0xC0) != 0x80 ? 1 : 0;
    }
    if (!isEventType(type) || characters == 0 || characters > 300) {
        result.error = "event needs a known type and 1-300 characters of text";
        return result;
    }
    if (respond && context.npcId.empty()) {
        result.error = "a responding event needs an NPC";
        return result;
    }

    json request = {
        {"protocol", kProtocol},
        {"request_id", requestId},
        {"session_id", config_.sessionId},
        {"type", type},
        {"text", text},
        {"context", context.details},
        {"respond", respond},
    };
    // The NPC is the one the game named; the server never picks it.
    if (!context.npcId.empty()) {
        request["npc"] = {{"id", context.npcId}, {"name", context.npcName}};
        request["player"] = {{"name", context.playerName}};
    }
    Response response = send(L"POST", "/event.php", "application/json", jsonBody(request));
    if (respond) {
        readTurnReply(response, context, result);
        return result;
    }
    result.error = response.failure();
    if (!result.error.empty()) {
        return result;
    }
    // Log only: the server stored it. No reply, history or actions exist for it.
    json reply = json::parse(response.body, nullptr, false);
    if (!reply.is_object() || reply["ok"] != true || reply["protocol"] != kProtocol
        || stringField(reply, "request_id") != requestId || reply["respond"] != false || reply["logged"] != true) {
        result.error = "unexpected reply from server";
        return result;
    }
    result.ok = true;
    return result;
}

void AiClient::readTurnReply(const Response& response, const GameContext& context, TurnResult& result) const
{
    result.error = response.failure();
    if (!result.error.empty()) {
        return;
    }

    // Check the shape of the reply before trusting anything in it.
    json reply = json::parse(response.body, nullptr, false);
    if (!reply.is_object() || reply["ok"] != true) {
        result.error = "unexpected reply from server";
        return;
    }
    if (reply["protocol"] != kProtocol) {
        result.error = "reply uses another protocol version; update the client or server";
        return;
    }
    if (stringField(reply, "request_id") != result.requestId) {
        result.error = "reply is for another request; ignored";
        return;
    }
    // Optional on older servers; when present it must be the NPC this request named.
    if (reply.contains("npc_id") && stringField(reply, "npc_id") != context.npcId) {
        result.error = "reply is for another NPC; ignored";
        return;
    }
    result.reply = stringField(reply, "reply");
    if (result.reply.empty() || result.reply.size() > kMaxReplyBytes) {
        result.error = "reply text is missing or too long";
        return;
    }
    // At most 4 distinct names, each kept once. The server never repeats a name, so a repeat
    // means a malformed reply: that name is rejected (one entry, no handler) whatever its copies say.
    if (reply["actions"].is_array()) {
        for (const json& action : reply["actions"]) {
            TurnAction read = readAction(action, context.npcId);
            TurnAction* earlier = nullptr;
            for (TurnAction& kept : result.actions) {
                if (!read.name.empty() && kept.name == read.name) {
                    earlier = &kept;
                }
            }
            if (earlier) {
                earlier->problem = "action repeated in the reply";
                continue;
            }
            if (result.actions.size() >= 4) {
                break;  // protocol 1 returns at most a few actions; ignore the rest
            }
            result.actions.push_back(read);
        }
    }
    // Optional field; older servers do not send it.
    if (reply.contains("logged") && reply["logged"] == false) {
        result.logged = false;
    }
    result.ok = true;
}

std::string AiClient::cancel(const std::string& npcId) const
{
    json request = {{"protocol", kProtocol}, {"session_id", config_.sessionId}, {"npc_id", npcId}};
    return send(L"POST", "/cancel.php", "application/json", jsonBody(request)).failure();
}

std::string AiClient::reportResult(const std::string& requestId, const std::string& npcId,
                                  const std::vector<ActionReport>& reports, bool& unsupported) const
{
    unsupported = false;
    json results = json::array();
    for (const ActionReport& report : reports) {
        json item = {{"name", report.name}, {"status", report.status}};
        if (!report.reason.empty()) {
            item["reason"] = report.reason;
        }
        results.push_back(item);
    }
    json request = {{"protocol", kProtocol}, {"request_id", requestId}, {"session_id", config_.sessionId},
                    {"npc_id", npcId}, {"results", results}};
    Response response = send(L"POST", "/result.php", "application/json", jsonBody(request));
    if (!response.error.empty()) {
        return response.error;
    }
    if (response.status == 200) {
        return "";
    }
    // A 404 without this protocol's error body means the endpoint does not exist.
    json reply = json::parse(response.body, nullptr, false);
    if (response.status == 404 && !(reply.is_object() && reply["protocol"] == kProtocol)) {
        unsupported = true;
        return "server has no result.php";
    }
    return describeFailure(response.status, response.body);
}

DecisionResult AiClient::decide(const std::string& transcript, const std::vector<DecisionCandidate>& candidates,
                               const std::string& baselineId) const
{
    DecisionResult result;
    result.requestId = newRequestId("dec");
    json offered = json::array();
    for (const DecisionCandidate& candidate : candidates) {
        json item = {{"id", candidate.id}};
        if (!candidate.name.empty()) {
            item["name"] = candidate.name;
        }
        if (!candidate.cues.empty()) {
            item["cues"] = candidate.cues;
        }
        offered.push_back(item);
    }
    // request_id is optional (added within protocol 1); older servers ignore it.
    json request = {{"protocol", kProtocol}, {"request_id", result.requestId}, {"transcript", transcript},
                    {"baseline_id", baselineId}, {"candidates", offered}};
    Response response = send(L"POST", "/decision.php", "application/json", jsonBody(request));
    result.error = response.failure();
    if (!result.error.empty()) {
        return result;
    }

    // Accept only an id this game offered.
    json reply = json::parse(response.body, nullptr, false);
    if (!reply.is_object() || reply["protocol"] != kProtocol || reply["ok"] != true) {
        result.error = "unexpected reply from server";
        return result;
    }
    // Optional on older servers; when present it must be the id this request sent.
    if (reply.contains("request_id") && stringField(reply, "request_id") != result.requestId) {
        result.error = "reply is for another request; ignored";
        return result;
    }
    result.chosenId = stringField(reply, "chosen_id");
    result.source = stringField(reply, "source");
    result.reason = stringField(reply, "reason");
    for (const DecisionCandidate& candidate : candidates) {
        if (candidate.id == result.chosenId) {
            result.ok = true;
        }
    }
    if (!result.ok) {
        result.error = "server chose an id that was not offered";
    }
    return result;
}

bool AiClient::speak(const std::string& text, const std::filesystem::path& wavFile, std::string& error,
                     const std::string& requestId, const std::string& parentRequestId, const std::string& npcId) const
{
    json request = {{"protocol", kProtocol}, {"text", text}};
    // Optional fields (added within protocol 1). npc_id only selects the NPC's profile voice.
    if (isSafe(requestId, 8, 64, true)) {
        request["request_id"] = requestId;
    }
    if (isSafe(parentRequestId, 8, 64, true)) {
        request["parent_request_id"] = parentRequestId;
    }
    if (isValidId(npcId)) {
        request["npc_id"] = npcId;
    }
    Response response = send(L"POST", "/speak.php", "application/json", jsonBody(request));
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    if (response.status != 200 || !isWav(response.body)) {
        error = describeFailure(response.status, response.body);
        return false;
    }

    std::error_code ignored;
    std::filesystem::create_directories(wavFile.parent_path(), ignored);
    std::ofstream out(wavFile, std::ios::binary);
    out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    if (!out) {
        error = "cannot write " + wavFile.u8string();
        return false;
    }
    return true;
}

bool AiClient::listen(const std::filesystem::path& wavFile, std::string& text, std::string& error,
                      const std::string& requestId, const std::string& parentRequestId) const
{
    std::ifstream in(wavFile, std::ios::binary);
    std::string audio((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!isWav(audio)) {
        error = "cannot read a WAV file at " + wavFile.u8string();
        return false;
    }

    // The body is audio, so the optional ids travel as headers. Only checked ids are sent, so
    // nothing else can reach the header block.
    std::string headers;
    if (isSafe(requestId, 8, 64, true)) {
        headers += "X-Request-Id: " + requestId + "\r\n";
    }
    if (isSafe(parentRequestId, 8, 64, true)) {
        headers += "X-Parent-Request-Id: " + parentRequestId + "\r\n";
    }
    Response response = send(L"POST", "/listen.php", "audio/wav", audio, headers);
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    json reply = json::parse(response.body, nullptr, false);
    if (response.status != 200 || !reply.is_object() || reply["ok"] != true) {
        error = describeFailure(response.status, response.body);
        return false;
    }
    // Optional on older servers; when present it must be the id this request sent.
    if (!requestId.empty() && reply.contains("request_id") && stringField(reply, "request_id") != requestId) {
        error = "reply is for another request; ignored";
        return false;
    }
    text = stringField(reply, "text");
    return true;
}
