#pragma once
#include "json.hpp"
#include <map>
#include <string>
#include <vector>

namespace ce {

// Saves / loads shape-parameter presets as JSON files in the presets directory.
// Format: { "model": "<file>", "values": { "<param id>": <0..1> },
//           "clothing": [ { "path", "fitScale", "fitOffset", "fitRot", "visible",
//                          "type", "slot" } ] }
class Presets {
public:
    explicit Presets(std::string dir = "presets") : dir_(std::move(dir)) {}

    bool save(const std::string& presetName, const std::string& modelName,
              const std::map<std::string, float>& values,
              const nlohmann::json& clothing, std::string& error) const;
    bool load(const std::string& presetName, std::map<std::string, float>& outValues,
              nlohmann::json& outClothing, std::string& error) const;
    bool remove(const std::string& presetName) const;
    std::vector<std::string> list() const;

private:
    std::string dir_;
    std::string pathFor(const std::string& presetName) const;
};

} // namespace ce
