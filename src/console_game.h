#pragma once
// A fake "game" that prints to the console. It only exists to show the
// GameAdapter boundary; it is not a plugin for any real game.
// Two fixed fictional NPCs stand in the scene. /npc (see main.cpp) chooses who the player
// faces; /unload and /load simulate that NPC leaving and a new instance arriving.

#include <iostream>
#include <map>
#include <string>

#include "game_adapter.h"

class ConsoleGame : public GameAdapter {
public:
    ConsoleGame()
    {
        npcs_["npc_guide"] = {"Guide", "Market Square"};
        npcs_["npc_scout"] = {"Scout", "North Gate"};
    }

    GameContext readContext() override
    {
        GameContext context;
        context.playerName = "Traveler";
        const Npc& npc = npcs_.at(current_);
        if (!npc.loaded) {
            return context;  // no NPC to talk to
        }
        context.npcId = current_;
        context.npcName = npc.name;
        context.npcInstance = npc.instance;
        context.details["location"] = npc.location;
        context.details["time_of_day"] = npc.following ? "Evening" : "Morning";
        return context;
    }

    void showSubtitle(const std::string& speaker, const std::string& text) override
    {
        std::cout << "[" << speaker << "] " << text << "\n";
    }

    void showMessage(const std::string& text) override
    {
        std::cout << "* " << text << "\n";
    }

    bool isSameNpc(const GameContext& captured) override
    {
        auto npc = npcs_.find(captured.npcId);
        return npc != npcs_.end() && npc->second.loaded && captured.npcInstance == npc->second.instance;
    }

    // Console-only test seams, not part of GameAdapter.
    // Faces another fixed NPC ("guide" or "scout"). Returns false for any other name.
    bool faceNpc(const std::string& name)
    {
        std::string id = "npc_" + name;
        if (npcs_.count(id) == 0) {
            return false;
        }
        current_ = id;
        std::cout << "* Now facing " << npcs_.at(id).name << " (" << id << ", simulated).\n";
        return true;
    }
    void unloadNpc()
    {
        Npc& npc = npcs_.at(current_);
        npc.loaded = false;
        npc.following = false;
        std::cout << "* " << current_ << " unloaded (simulated).\n";
    }
    void loadNpc()
    {
        Npc& npc = npcs_.at(current_);
        if (!npc.loaded) {
            npc.loaded = true;
            ++npc.instance;
        }
        std::cout << "* " << current_ << " loaded as instance " << npc.instance << " (simulated).\n";
    }

    void followPlayer(const std::string& npcId) override
    {
        npcs_.at(npcId).following = true;
        std::cout << "* " << npcId << " is now following you (simulated).\n";
    }

    void playVoice(const std::filesystem::path& wavFile) override
    {
        std::cout << "* Voice saved to " << wavFile.u8string() << " (a real game would play it on the NPC).\n";
    }

private:
    struct Npc {
        std::string name;
        std::string location;
        bool loaded = true;
        bool following = false;
        unsigned long long instance = 1;
    };
    std::map<std::string, Npc> npcs_;
    std::string current_ = "npc_guide";
};
