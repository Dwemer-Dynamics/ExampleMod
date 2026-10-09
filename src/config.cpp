#include "config.h"

#include <fstream>

#include <nlohmann/json.hpp>

bool loadConfigFile(const std::filesystem::path& path, Config& config, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return true;
    }

    nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        error = path.u8string() + " is not a valid JSON object.";
        return false;
    }

    try {
        config.serverUrl = json.value("server_url", config.serverUrl);
        config.token = json.value("token", config.token);
        config.sessionId = json.value("session_id", config.sessionId);
        config.timeoutSeconds = json.value("timeout_seconds", config.timeoutSeconds);
        config.voiceEnabled = json.value("voice_enabled", config.voiceEnabled);
        config.allowedActions = json.value("allowed_actions", config.allowedActions);
    } catch (const nlohmann::json::exception&) {
        error = path.u8string() + " has a setting with the wrong type.";
        return false;
    }

    while (!config.serverUrl.empty() && config.serverUrl.back() == '/') {
        config.serverUrl.pop_back();
    }
    if (config.timeoutSeconds < 1 || config.timeoutSeconds > 300) {
        config.timeoutSeconds = 40;
    }
    return true;
}
