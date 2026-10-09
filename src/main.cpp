// Console "fake game" that talks to the example AI server.
//
// The main thread plays the role of the game thread: it runs a small tick loop
// and is the only thread that calls the GameAdapter. Network calls run on worker
// threads and hand their results back through a mailbox. A real engine plugin
// should follow the same rule.
//
// Usage:
//   example_mod.exe                       interactive console (/unload and /load simulate the NPC leaving)
//   example_mod.exe --version             print the client version and exit (no config needed)
//   example_mod.exe --health              check the server and exit
//   example_mod.exe --say "Follow me"     send one line and exit
//   example_mod.exe --listen speech.wav   optional voice: transcribe, send, exit
//   example_mod.exe --decide "Scout?"     optional: ask which NPC should answer, print it, exit
//   example_mod.exe --event item_given "The player gave a lantern."   log-only game event, exit
//   example_mod.exe --react item_given "The player gave a lantern."   the Guide reacts to it, exit
//   add --config my.json to use another private config file

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "ai_client.h"
#include "config.h"
#include "console_game.h"

namespace {

// A server cancellation that finished (error is "" when the server confirmed it).
struct CancelDone {
    std::string sessionId;
    std::string npcId;
    unsigned long long id = 0;
    std::string error;
};

// Hand-off between threads. Only the main thread reads from it.
struct Mailbox {
    std::mutex mutex;
    std::deque<std::string> lines;      // typed by the player
    std::deque<TurnResult> results;     // finished turns
    std::deque<CancelDone> cancels;     // finished server cancellations
    std::deque<std::string> messages;   // status from other workers
    bool inputClosed = false;
    bool reportsOff = false;            // set when the server has no result.php
};

std::string toUtf8(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

std::filesystem::path exeFolder()
{
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

bool isAllowed(const Config& config, const std::string& action)
{
    const std::vector<std::string>& allowed = config.allowedActions;
    return std::find(allowed.begin(), allowed.end(), action) != allowed.end();
}

// Status line for a log-only game event (event.php with respond false).
std::string describeLoggedEvent(const TurnResult& result, const std::string& type)
{
    if (!result.ok) {
        return "Event not logged (not retried; request " + result.requestId + "): " + result.error;
    }
    return "Event " + type + " logged by the server (log only; no reply or actions; request " + result.requestId + ").";
}

// Status line for a finished server cancellation. Failed cancels are never retried.
std::string describeCancel(const CancelDone& done)
{
    if (!done.error.empty()) {
        return "Server cancel failed (not retried; you can send again): " + done.error;
    }
    return "Server confirmed the cancel for " + done.npcId + " in session " + done.sessionId + ".";
}

// Runs on a worker thread: optional speech-to-text, the AI turn, then optional voice.
// With eventType set, the line is an explicit game event the NPC reacts to (event.php,
// respond true); everything after the request is the same as for a typed line.
TurnResult runTurn(const AiClient& client, const std::string& requestId, const GameContext& context,
                   std::string text, const std::filesystem::path& speechWav, const std::string& eventType = "")
{
    const Config& config = client.config();
    std::string heard;
    // Voice calls get their own request ids, with this turn's id as their parent, so the
    // server's Connector calls page can show them under the turn.
    std::string listenRequestId;
    if (!speechWav.empty()) {
        std::string error;
        listenRequestId = newRequestId("stt");
        if (!client.listen(speechWav, text, error, listenRequestId, requestId)) {
            TurnResult failed;
            failed.requestId = requestId;
            failed.listenRequestId = listenRequestId;
            failed.error = "speech to text failed (" + error + "), please type instead";
            return failed;
        }
        heard = text;
        if (text.empty()) {
            TurnResult failed;
            failed.requestId = requestId;
            failed.listenRequestId = listenRequestId;
            failed.error = "no speech was recognized, please type instead";
            return failed;
        }
    }

    TurnResult result = eventType.empty() ? client.sendTurn(requestId, context, text)
                                          : client.sendEvent(requestId, context, eventType, text, true);
    result.heard = heard;
    result.listenRequestId = listenRequestId;
    if (result.ok && config.voiceEnabled) {
        std::filesystem::path wav = std::filesystem::temp_directory_path() / "dwemer-ai-example" / (requestId + ".wav");
        std::string error;
        result.speakRequestId = newRequestId("tts");
        if (client.speak(result.reply, wav, error, result.speakRequestId, requestId, context.npcId)) {
            result.voiceFile = wav;
        } else {
            result.voiceError = error;
        }
    }
    return result;
}

// Optional NPC selection demo with two fictional NPCs; the guide is the baseline.
// It only reports the choice. It never switches the NPC being talked to or runs actions.
std::string describeDecision(const AiClient& client, const std::string& transcript)
{
    static const std::vector<DecisionCandidate> candidates = {
        {"npc_guide", "Guide", "market guide in the town square"},
        {"npc_scout", "Scout", "scout who knows the north trail"},
    };
    DecisionResult decision = client.decide(transcript, candidates, "npc_guide");
    if (!decision.ok) {
        return "Decision unavailable, keeping npc_guide (request " + decision.requestId + "): " + decision.error;
    }
    return "Decision: " + decision.chosenId + " (" + decision.source + ", " + decision.reason + ", request "
        + decision.requestId + "). Display only; still talking to the same NPC.";
}

// One line naming every request id of a turn, printed on success and failure, so the server's
// Logs and Connector calls pages can be searched for them.
std::string traceLine(const TurnResult& result)
{
    std::string line = "Trace: turn " + result.requestId;
    if (!result.listenRequestId.empty()) {
        line += ", speech-to-text " + result.listenRequestId;
    }
    if (!result.speakRequestId.empty()) {
        line += ", voice " + result.speakRequestId;
    }
    return line + (result.ok ? " (ok)" : " (failed)");
}

// Runs on the game thread. Only allowlisted actions with a known handler reach the game.
// Returns what happened to each returned action, for the optional result report.
std::vector<ActionReport> applyResult(GameAdapter& game, const Config& config, const GameContext& context,
                                      const TurnResult& result)
{
    std::vector<ActionReport> reports;
    if (!result.heard.empty()) {
        game.showMessage("You said: " + result.heard);
    }
    if (!result.ok) {
        // Never retried automatically: a retry could repeat a reply or an action.
        game.showMessage("AI unavailable: " + result.error + " (not retried; send again for a new request)");
        game.showMessage(traceLine(result));
        return reports;
    }
    game.showSubtitle(context.npcName, result.reply);
    for (const TurnAction& action : result.actions) {
        bool reportable = reports.size() < 4 && !action.name.empty();  // result.php limits
        if (!action.problem.empty()) {
            game.showMessage("Rejected action " + (action.name.empty() ? std::string("(malformed)") : action.name)
                             + ": " + action.problem);
            if (reportable) {
                reports.push_back({action.name, "rejected", action.problem});
            }
        } else if (action.name == "follow_player" && isAllowed(config, action.name)) {
            // The target is the NPC this request was sent to; a sent target_id was checked
            // against it already.
            game.followPlayer(context.npcId);
            if (reportable) {
                reports.push_back({action.name, "handled", ""});
            }
        } else {
            game.showMessage("Rejected action not allowed by this mod: " + action.name);
            if (reportable) {
                reports.push_back({action.name, "rejected", "not allowed by this mod"});
            }
        }
    }
    if (!result.voiceFile.empty()) {
        game.playVoice(result.voiceFile);
    } else if (!result.voiceError.empty()) {
        game.showMessage("Voice unavailable, text only: " + result.voiceError);
    }
    game.showMessage(traceLine(result));
    return reports;
}

// Runs on a worker thread (or after the turn in --say). Returns a status line for the game.
// A failed report never changes the reply or actions already applied.
std::string sendReport(const AiClient& client, const std::string& requestId, const std::string& npcId,
                       const std::vector<ActionReport>& reports, bool& unsupported)
{
    std::string error = client.reportResult(requestId, npcId, reports, unsupported);
    if (unsupported) {
        return "Result reports off: this server has no result.php.";
    }
    if (!error.empty()) {
        return "Result report failed (reply and actions kept): " + error;
    }
    std::string names;
    for (const ActionReport& report : reports) {
        names += (names.empty() ? "" : ", ") + report.name + " " + report.status;
    }
    // This is what the console client did, not a receipt from a real game engine.
    return "Result reported to server: " + names + " (client-reported, console simulation).";
}

// "/event <type> <text>": splits off the type. Returns false when either part is missing.
bool splitEvent(const std::string& rest, std::string& type, std::string& text)
{
    size_t space = rest.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    type = rest.substr(0, space);
    text = rest.substr(space + 1);
    return !type.empty() && !text.empty();
}

// Each request keeps the client (and so the session and settings) it was started with.
// /session makes a new client; requests already out keep using the old one.
using ClientPtr = std::shared_ptr<const AiClient>;

// console is used only for the /npc, /unload and /load test commands; everything else goes
// through the GameAdapter interface, as a real engine port would.
int runInteractive(ClientPtr client, GameAdapter& game, ConsoleGame& console)
{
    auto mailbox = std::make_shared<Mailbox>();
    game.showMessage("Type to talk. Commands: /cancel  /listen <file.wav>  /decide <text>  /event <type> <text>"
                     "  /react <type> <text>  /npc guide|scout  /session <id>  /unload  /load  /quit");

    // getline blocks. Shared ownership keeps its mailbox alive until input ends.
    // /quit also ends this thread; network workers are joined below.
    std::thread([mailbox] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->lines.push_back(line);
            if (line == "/quit") {
                break;
            }
        }
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        mailbox->inputClosed = true;
    }).detach();

    std::vector<std::future<void>> workers;
    constexpr size_t maxWorkers = 4;  // turn, event, decision, voice and report requests
    // Server cancellations have their own small pool, so a cancel is sent at once even while
    // every normal worker waits on a slow provider. A new turn starts only while a cancel slot
    // stays free for it, so dropping the pending turn can always send its cancel. Bounded:
    // at most maxWorkers + maxCancelWorkers threads.
    std::vector<std::future<void>> cancelWorkers;
    constexpr size_t maxCancelWorkers = 2;
    std::string pendingRequestId;  // the only turn whose result we still want
    std::string lastAppliedRequestId;  // a completed reply is applied at most once
    GameContext pendingContext;
    ClientPtr pendingClient;  // the client (session, allowlist) the pending turn was sent with
    // Server cancels still out, by session and NPC id. A cancel makes every turn the server has
    // for that NPC in that session stale, so a new turn there sent before it is answered could be
    // cancelled by it. New turns to that NPC in that session wait until it is answered or fails.
    // Other NPCs and sessions are not held up. Only the newest cancel's answer clears it.
    std::map<std::pair<std::string, std::string>, unsigned long long> cancelling;
    unsigned long long lastCancelId = 0;

    // Tells the server to drop the in-flight turn, in the session it was sent in. The reply is
    // already discarded locally. Returns false when no cancel slot was free (see maxCancelWorkers;
    // not expected for the pending turn). A failed cancel is reported, never retried.
    auto queueCancel = [&](const ClientPtr& sender, const std::string& npcId) {
        if (cancelWorkers.size() >= maxCancelWorkers) {
            return false;
        }
        unsigned long long id = ++lastCancelId;
        std::string sessionId = sender->config().sessionId;
        cancelling[{sessionId, npcId}] = id;
        cancelWorkers.push_back(std::async(std::launch::async, [sender, mailbox, sessionId, npcId, id] {
            std::string error = sender->cancel(npcId);
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->cancels.push_back({sessionId, npcId, id, error});
        }));
        return true;
    };
    // Forgets the pending turn, shows message and cancels the turn on the server. Used by
    // /cancel, /quit, /npc, /session and when the NPC leaves. Returns false if nothing was pending.
    auto dropPending = [&](const std::string& message) {
        if (pendingRequestId.empty()) {
            return false;
        }
        pendingRequestId.clear();
        game.showMessage(message);
        if (!queueCancel(pendingClient, pendingContext.npcId)) {
            game.showMessage("Cancel slots busy; reply discarded locally. Server cancellation was not sent.");
        }
        return true;
    };

    auto handleResult = [&](const TurnResult& result) {
        if (!lastAppliedRequestId.empty() && result.requestId == lastAppliedRequestId) {
            game.showMessage("Ignored a second copy of a reply that was already applied.");
            return;
        }
        if (result.requestId != pendingRequestId) {
            game.showMessage("Ignored a stale reply (request " + result.requestId + "; it was cancelled or replaced).");
            return;
        }
        pendingRequestId.clear();
        // Recheck on the game thread right before applying: the NPC may have been unloaded,
        // deleted or replaced while the request was out. Nothing is applied or reported then.
        if (!game.isSameNpc(pendingContext)) {
            game.showMessage("Dropped the reply and its actions: " + pendingContext.npcName + " is no longer here (request "
                             + result.requestId + ").");
            return;
        }
        lastAppliedRequestId = result.requestId;
        std::vector<ActionReport> reports = applyResult(game, pendingClient->config(), pendingContext, result);
        bool reportsOff = false;
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            reportsOff = mailbox->reportsOff;
        }
        // Optional report, queued after the game thread applied the actions. Skipped when
        // there is nothing to report or the server could not log the turn.
        if (reports.empty() || !result.logged || reportsOff) {
            return;
        }
        if (workers.size() >= maxWorkers) {
            game.showMessage("Workers busy; result report skipped (reply and actions kept).");
            return;
        }
        workers.push_back(std::async(std::launch::async, [sender = pendingClient, mailbox, requestId = result.requestId,
                                                           npcId = pendingContext.npcId, reports] {
            bool unsupported = false;
            std::string message = sendReport(*sender, requestId, npcId, reports, unsupported);
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->reportsOff = mailbox->reportsOff || unsupported;
            mailbox->messages.push_back(message);
        }));
    };

    bool running = true;
    auto reap = [](std::vector<std::future<void>>& pool) {
        for (auto worker = pool.begin(); worker != pool.end();) {
            if (worker->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                worker->get();
                worker = pool.erase(worker);
            } else {
                ++worker;
            }
        }
    };
    while (running) {
        reap(workers);
        reap(cancelWorkers);
        std::deque<std::string> lines;
        std::deque<TurnResult> results;
        std::deque<CancelDone> cancels;
        std::deque<std::string> messages;
        bool inputClosed = false;
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            lines.swap(mailbox->lines);
            results.swap(mailbox->results);
            cancels.swap(mailbox->cancels);
            messages.swap(mailbox->messages);
            inputClosed = mailbox->inputClosed;
        }

        for (const std::string& message : messages) {
            game.showMessage(message);
        }
        for (const CancelDone& done : cancels) {
            auto waiting = cancelling.find({done.sessionId, done.npcId});
            if (waiting != cancelling.end() && waiting->second == done.id) {
                cancelling.erase(waiting);
            }
            game.showMessage(describeCancel(done));
        }
        for (const std::string& line : lines) {
            if (line.empty()) {
                continue;
            }
            if (line == "/quit") {
                // Shutdown: the pending reply is never applied, and the server is told so it
                // saves no history for it.
                dropPending("Quitting: dropped the pending reply for " + pendingContext.npcName + ".");
                running = false;
                break;
            }
            if (line == "/cancel") {
                if (!dropPending("Cancelled.")) {
                    game.showMessage("Nothing to cancel.");
                }
                continue;
            }
            if (line == "/unload") {
                console.unloadNpc();
                continue;
            }
            if (line == "/load") {
                console.loadNpc();
                continue;
            }
            if (line.rfind("/npc ", 0) == 0) {
                if (!console.faceNpc(line.substr(5))) {
                    game.showMessage("Unknown NPC. Use /npc guide or /npc scout.");
                    continue;
                }
                // The player turned away: the other NPC's reply must not arrive later.
                if (!pendingRequestId.empty() && game.readContext().npcId != pendingContext.npcId) {
                    dropPending("Dropped the pending reply for " + pendingContext.npcName + ": you turned to another NPC.");
                }
                continue;
            }
            if (line.rfind("/session ", 0) == 0) {
                std::string sessionId = line.substr(9);
                if (!isValidId(sessionId)) {
                    game.showMessage("Session ids are 1-64 characters of A-Z a-z 0-9 _ . : -");
                    continue;
                }
                if (sessionId == client->config().sessionId) {
                    game.showMessage("Already in session " + sessionId + ".");
                    continue;
                }
                dropPending("Dropped the pending reply for " + pendingContext.npcName + ": the session changed.");
                Config next = client->config();
                next.sessionId = sessionId;
                client = std::make_shared<const AiClient>(next);
                // This client never reads or changes game saves. Server checkpoints are matched
                // to saves by hand, by this id.
                game.showMessage("Session is now " + sessionId + " (manual, like loading another save). "
                                 "Server checkpoints for it: php scripts/checkpoint.php list " + sessionId);
                continue;
            }

            if (line.rfind("/decide ", 0) == 0) {
                if (workers.size() >= maxWorkers) {
                    game.showMessage("Workers busy; please try again when a request finishes.");
                    continue;
                }
                workers.push_back(std::async(std::launch::async, [client, mailbox, transcript = line.substr(8)] {
                    std::string message = describeDecision(*client, transcript);
                    std::lock_guard<std::mutex> lock(mailbox->mutex);
                    mailbox->messages.push_back(message);
                }));
                continue;
            }
            if (line.rfind("/event ", 0) == 0) {
                // Log only: no reply, history or actions, so nothing waits for it.
                std::string type;
                std::string text;
                if (!splitEvent(line.substr(7), type, text) || !isEventType(type)) {
                    game.showMessage("Use /event <type> <text>; types: location_entered item_given combat_started combat_ended");
                    continue;
                }
                if (workers.size() >= maxWorkers) {
                    game.showMessage("Workers busy; please try again when a request finishes.");
                    continue;
                }
                workers.push_back(std::async(std::launch::async, [client, mailbox, context = game.readContext(), type, text] {
                    TurnResult result = client->sendEvent(newRequestId("evt"), context, type, text, false);
                    std::string message = describeLoggedEvent(result, type);
                    std::lock_guard<std::mutex> lock(mailbox->mutex);
                    mailbox->messages.push_back(message);
                }));
                continue;
            }

            std::string text = line;
            std::string eventType;
            std::filesystem::path speechWav;
            if (line.rfind("/listen ", 0) == 0) {
                text.clear();
                speechWav = std::filesystem::u8path(line.substr(8));
            } else if (line.rfind("/react ", 0) == 0) {
                if (!splitEvent(line.substr(7), eventType, text) || !isEventType(eventType)) {
                    game.showMessage("Use /react <type> <text>; types: location_entered item_given combat_started combat_ended");
                    continue;
                }
            } else if (line[0] == '/') {
                game.showMessage("Unknown command.");
                continue;
            }

            // A new turn replaces any pending one; the server marks the old one stale.
            if (workers.size() >= maxWorkers) {
                game.showMessage("Workers busy; please try again when a request finishes.");
                continue;
            }
            GameContext context = game.readContext();
            if (context.npcId.empty()) {
                game.showMessage("No NPC here to talk to.");
                continue;
            }
            if (cancelling.count({client->config().sessionId, context.npcId}) > 0) {
                game.showMessage("Waiting for the server to confirm the cancel for " + context.npcName
                                 + "; send again after it answers (not queued).");
                continue;
            }
            // Keep one cancel slot free for this new turn, after any cancel for the old one.
            bool replacesOther = !pendingRequestId.empty() && context.npcId != pendingContext.npcId;
            if (cancelWorkers.size() + (replacesOther ? 1 : 0) >= maxCancelWorkers) {
                game.showMessage("Waiting for a server cancel to answer; send again after it does (not queued).");
                continue;
            }
            if (replacesOther && !queueCancel(pendingClient, pendingContext.npcId)) {
                // Not expected: the check above left a slot. The old reply is not wanted either way.
                game.showMessage("Cancel slots busy; old reply discarded locally. Server cancellation was not sent.");
            }
            pendingContext = context;
            pendingClient = client;
            pendingRequestId = newRequestId();
            game.showMessage("(waiting for " + pendingContext.npcName + "...)");
            workers.push_back(std::async(std::launch::async, [sender = client, mailbox, requestId = pendingRequestId,
                                                               context = pendingContext, text, speechWav, eventType] {
                TurnResult result = runTurn(*sender, requestId, context, text, speechWav, eventType);
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                mailbox->results.push_back(result);
            }));
        }
        // If the NPC left while its reply is out, drop the reply now and tell the server.
        if (running && !pendingRequestId.empty() && !game.isSameNpc(pendingContext)) {
            dropPending("Dropped the pending reply for " + pendingContext.npcName + ": " + pendingContext.npcName
                        + " is no longer here.");
        }
        if (running) {
            for (const TurnResult& result : results) {
                handleResult(result);
            }
        }

        if (inputClosed && pendingRequestId.empty()) {
            running = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Quit drops late replies and actions. At most maxWorkers + maxCancelWorkers requests are
    // out and each blocking HTTP call ends within its timeouts, so this wait is bounded. Nothing they return is applied.
    pendingRequestId.clear();
    if (!workers.empty() || !cancelWorkers.empty()) {
        game.showMessage("Waiting for " + std::to_string(workers.size() + cancelWorkers.size())
                         + " request(s) to finish; replies are not applied.");
    }
    for (std::future<void>& worker : cancelWorkers) {
        worker.get();
    }
    for (std::future<void>& worker : workers) {
        worker.get();
    }
    std::lock_guard<std::mutex> lock(mailbox->mutex);
    for (const CancelDone& done : mailbox->cancels) {
        game.showMessage(describeCancel(done));
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(toUtf8(argv[i]));
    }
    if (args.size() == 1 && args[0] == "--version") {
        std::cout << "example_mod " << kClientVersion << " (" << kClientBaseline << ", protocol 1)\n";
        return 0;
    }

    // Defaults, then the shared default file, then the private config.
    Config config;
    std::filesystem::path folder = exeFolder();
    std::filesystem::path privateConfig = folder / "config.json";
    std::string mode;
    std::string value;
    std::string eventType;
    for (size_t i = 0; i < args.size(); ++i) {
        bool hasValue = i + 1 < args.size();
        if (args[i] == "--config" && hasValue) {
            privateConfig = std::filesystem::u8path(args[++i]);
        } else if ((args[i] == "--say" || args[i] == "--listen" || args[i] == "--decide") && hasValue) {
            mode = args[i];
            value = args[++i];
        } else if ((args[i] == "--event" || args[i] == "--react") && i + 2 < args.size()) {
            mode = args[i];
            eventType = args[++i];
            value = args[++i];
        } else if (args[i] == "--health") {
            mode = args[i];
        } else {
            std::cerr << "Unknown argument: " << args[i] << "\n";
            return 2;
        }
    }

    std::string error;
    if (!loadConfigFile(folder / "config.default.json", config, error)
        || !loadConfigFile(privateConfig, config, error)) {
        std::cerr << "Config error: " << error << "\n";
        return 2;
    }
    if (config.token.empty()) {
        std::cerr << "No token set. Copy config.default.json to config.json next to the exe and paste the\n"
                     "token from the server's config/config.php, or pass --config <file>.\n";
        return 2;
    }

    auto shared = std::make_shared<const AiClient>(config);
    const AiClient& client = *shared;
    ConsoleGame game;

    if (mode == "--health") {
        std::string note;
        std::string info;
        std::string problem = client.health(&note, &info);
        if (!problem.empty()) {
            std::cout << "Server problem: " << problem << "\n";
            return 1;
        }
        std::cout << "Server is ready.\n";
        std::cout << "Versions: client " << kClientVersion << ", client baseline " << kClientBaseline << ", " << info
                  << ", protocol 1.\n";
        if (!note.empty()) {
            std::cout << "Note: " << note << "\n";
        }
        return 0;
    }
    if (mode == "--event") {
        TurnResult result = client.sendEvent(newRequestId("evt"), game.readContext(), eventType, value, false);
        game.showMessage(describeLoggedEvent(result, eventType));
        return result.ok ? 0 : 1;
    }
    if (mode == "--decide") {
        std::string message = describeDecision(client, value);
        game.showMessage(message);
        return message.rfind("Decision:", 0) == 0 ? 0 : 1;
    }
    if (mode == "--say" || mode == "--listen" || mode == "--react") {
        GameContext context = game.readContext();
        std::string text = mode == "--listen" ? "" : value;
        std::filesystem::path speechWav = mode == "--listen" ? std::filesystem::u8path(value) : std::filesystem::path();
        TurnResult result = runTurn(client, newRequestId(), context, text, speechWav, eventType);
        std::vector<ActionReport> reports = applyResult(game, config, context, result);
        if (!reports.empty() && result.logged) {
            bool unsupported = false;
            game.showMessage(sendReport(client, result.requestId, context.npcId, reports, unsupported));
        }
        return result.ok ? 0 : 1;
    }
    return runInteractive(shared, game, game);
}
