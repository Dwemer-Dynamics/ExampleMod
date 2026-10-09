#pragma once
// The boundary between the AI client and a game.
// Everything game-specific lives behind GameAdapter. To support a real engine,
// write a new class that implements it with that engine's own APIs.
// console_game.h is the fake console implementation used by this example.

#include <filesystem>
#include <map>
#include <string>

// What the game tells the AI about the current moment.
struct GameContext {
    std::string playerName;
    std::string npcId;    // stable id for this NPC, e.g. an engine reference id
    std::string npcName;
    std::map<std::string, std::string> details;  // sent as "context", e.g. location
    // Engine-specific marker of this NPC instance, e.g. a handle or load count. Captured with
    // the request and compared by isSameNpc() before a reply or action is applied.
    unsigned long long npcInstance = 0;
};

// Every method is called from the game's main thread only (see the loop in main.cpp).
class GameAdapter {
public:
    virtual ~GameAdapter() = default;

    // Read the current player, the NPC being talked to, and a few facts about the scene.
    virtual GameContext readContext() = 0;

    // Show what the NPC says, e.g. as a subtitle.
    virtual void showSubtitle(const std::string& speaker, const std::string& text) = 0;

    // Show a short status or error message to the player.
    virtual void showMessage(const std::string& text) = 0;

    // True while the NPC captured in readContext() is still loaded and is the same instance.
    // Checked right before a reply or its actions are applied: an unloaded, deleted or
    // replaced NPC must never receive an old reply.
    virtual bool isSameNpc(const GameContext& captured) = 0;

    // The allowlisted "follow_player" action. Use the engine's own follow behaviour.
    // npcId is always the NPC the request was sent to, never a value chosen by the model.
    virtual void followPlayer(const std::string& npcId) = 0;

    // Optional voice: play a WAV file on the NPC.
    virtual void playVoice(const std::filesystem::path& wavFile) = 0;
};
