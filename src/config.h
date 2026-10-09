#pragma once
// Client settings. config.default.json is loaded first, then your private
// config.json (which holds the token) overrides any keys it contains.

#include <filesystem>
#include <string>
#include <vector>

struct Config {
    std::string serverUrl = "http://127.0.0.1:8081/ExampleServer";
    std::string token;
    std::string sessionId = "demo-save-1";
    int timeoutSeconds = 40;
    bool voiceEnabled = false;
    std::vector<std::string> allowedActions = {"follow_player"};
};

// A missing file is fine and changes nothing. Returns false with a message
// if the file exists but is not valid.
bool loadConfigFile(const std::filesystem::path& path, Config& config, std::string& error);
