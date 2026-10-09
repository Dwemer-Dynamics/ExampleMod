#pragma once
// Talks to the example server over HTTP (WinHTTP). See PROTOCOL.md.
// Every method blocks, so call them from a worker thread, never the game thread.
// Methods only read the config, so several threads may use one client.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "game_adapter.h"

// One returned action. targetId is args.target_id when the server sent it (it must equal the
// request's NPC); problem is set when the action's shape was not acceptable.
struct TurnAction {
    std::string name;
    std::string targetId;
    std::string problem;
};

struct TurnResult {
    std::string requestId;
    bool ok = false;
    std::string error;                 // set when ok is false
    std::string heard;                 // what speech-to-text heard, if voice input was used
    std::string reply;                 // what the NPC says
    std::vector<TurnAction> actions;   // the game still checks its own allowlist and handler
    bool logged = true;                // false if the server says it could not log this turn
    bool logOnly = false;              // a log-only game event: no reply, no actions
    std::filesystem::path voiceFile;   // optional voice, set when it worked
    std::string voiceError;            // optional voice, set when it failed
    std::string listenRequestId;       // request id of the speech-to-text call, if voice input was used
    std::string speakRequestId;        // request id of the text-to-speech call, if voice was tried
};

// Optional NPC selection (decision.php). The game offers the candidates; the server only
// picks one of them. Nothing here changes who the player is talking to.
struct DecisionCandidate {
    std::string id;
    std::string name;
    std::string cues;  // optional short hints, e.g. "north trail scout"
};

struct DecisionResult {
    std::string requestId; // sent as the optional request_id; find it in the server's Connector calls
    bool ok = false;
    std::string error;     // set when ok is false
    std::string chosenId;  // always one of the offered ids when ok
    std::string source;    // "provider" or "baseline"
    std::string reason;    // e.g. "matched", "chosen", "disabled", "low_confidence"
};

// Optional: what the game did with one returned action, sent to result.php.
// status is "handled", "rejected" or "failed". The server stores it as untrusted.
struct ActionReport {
    std::string name;
    std::string status;
    std::string reason;  // optional, short
};

class AiClient {
public:
    explicit AiClient(Config config) : config_(std::move(config)) {}

    // The settings this client was made with. They never change after construction.
    const Config& config() const { return config_; }

    // Returns "" when the server is ready, otherwise what is wrong. note, if given, gets a
    // remark about optional features an older server lacks.
    // info, if given, gets the server's version, baseline and schema when it reports them.
    std::string health(std::string* note = nullptr, std::string* info = nullptr) const;

    // Sends one player line to one NPC.
    TurnResult sendTurn(const std::string& requestId, const GameContext& context, const std::string& text) const;

    // Sends one game event (event.php). respond false: log only, the result has logOnly set
    // and no reply or actions. respond true: the NPC in context reacts, exactly like sendTurn.
    TurnResult sendEvent(const std::string& requestId, const GameContext& context, const std::string& type,
                         const std::string& text, bool respond) const;

    // Tells the server to drop any in-flight turn for this NPC. Returns "" on success.
    std::string cancel(const std::string& npcId) const;

    // Optional: reports what the game did with a finished turn's actions. Returns "" on
    // success. Sets unsupported when the server has no result.php (an older server).
    std::string reportResult(const std::string& requestId, const std::string& npcId,
                             const std::vector<ActionReport>& reports, bool& unsupported) const;

    // Optional: which offered NPC should answer this line. Falls back to baselineId on the server.
    DecisionResult decide(const std::string& transcript, const std::vector<DecisionCandidate>& candidates,
                          const std::string& baselineId) const;

    // Optional voice. Both return false and set error when voice is unavailable.
    // requestId names the call in the server's logs; parentRequestId is the turn it belongs to.
    // npcId (speak only) lets the server use that NPC's profile voice; it changes nothing else.
    // Ids that are not valid protocol ids are not sent (an older server ignores them anyway).
    bool speak(const std::string& text, const std::filesystem::path& wavFile, std::string& error,
               const std::string& requestId = "", const std::string& parentRequestId = "",
               const std::string& npcId = "") const;
    bool listen(const std::filesystem::path& wavFile, std::string& text, std::string& error,
                const std::string& requestId = "", const std::string& parentRequestId = "") const;

private:
    struct Response {
        int status = 0;
        std::string body;
        std::string error;  // transport problem, e.g. server not running

        // "" for an HTTP 200 reply, otherwise the transport error or a readable HTTP failure.
        std::string failure() const;
    };
    // extraHeaders: complete "Name: value\r\n" lines, built only from validated ids.
    Response send(const wchar_t* method, const std::string& path, const std::string& contentType,
                  const std::string& body, const std::string& extraHeaders = "") const;

    // Checks a turn-shaped reply (turn.php or event.php with respond true) and fills result.
    void readTurnReply(const Response& response, const GameContext& context, TurnResult& result) const;

    Config config_;
};

// A new random id for each request, e.g. "req-1a2b3c..." for a turn, "tts-..." for speech.
std::string newRequestId(const char* prefix = "req");

// This client's example baseline, shown by --health. Not a release, protocol or schema number.
constexpr const char* kClientBaseline = "baseline-1";
// This client's release version, from project(... VERSION) in CMakeLists.txt.
#ifndef EXAMPLE_CLIENT_VERSION
#error "Build with CMake: it sets EXAMPLE_CLIENT_VERSION from the project version."
#endif
constexpr const char* kClientVersion = EXAMPLE_CLIENT_VERSION;

// The protocol's id rule: 1-64 characters of A-Z a-z 0-9 _ . : -
bool isValidId(const std::string& id);

// One of the fixed protocol 1 event types (location_entered, item_given, combat_started,
// combat_ended).
bool isEventType(const std::string& type);
