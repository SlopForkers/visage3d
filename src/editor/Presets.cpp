#include "editor/Presets.h"
#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace ce {

std::string Presets::pathFor(const std::string& presetName) const {
    std::string safe = presetName;
    std::replace_if(safe.begin(), safe.end(),
                    [](char c) { return c == '/' || c == '\\' || c == ':' || c == '*'; },
                    '_');
    return dir_ + "/" + safe + ".json";
}

bool Presets::save(const std::string& presetName, const std::string& modelName,
                   const std::map<std::string, float>& values, std::string& error) const {
    try {
        std::filesystem::create_directories(dir_);
        nlohmann::json j;
        j["model"] = modelName;
        j["values"] = nlohmann::json::object();
        for (const auto& kv : values) j["values"][kv.first] = kv.second;
        std::ofstream f(pathFor(presetName));
        if (!f) { error = "Cannot write preset file"; return false; }
        f << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool Presets::load(const std::string& presetName, std::map<std::string, float>& outValues,
                   std::string& error) const {
    try {
        std::ifstream f(pathFor(presetName));
        if (!f) { error = "Preset not found: " + presetName; return false; }
        nlohmann::json j = nlohmann::json::parse(f);
        outValues.clear();
        if (j.contains("values"))
            for (auto it = j["values"].begin(); it != j["values"].end(); ++it)
                outValues[it.key()] = it.value().get<float>();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool Presets::remove(const std::string& presetName) const {
    std::error_code ec;
    return std::filesystem::remove(pathFor(presetName), ec);
}

std::vector<std::string> Presets::list() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".json")
            out.push_back(entry.path().stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace ce
