#pragma once
#include <map>
#include <string>
#include <vector>

namespace ce {

// Saves / loads shape-parameter presets as JSON files in the presets directory.
// Format: { "model": "<file name>", "values": { "<param id>": <0..1>, ... } }
class Presets {
public:
    explicit Presets(std::string dir = "presets") : dir_(std::move(dir)) {}

    bool save(const std::string& presetName, const std::string& modelName,
              const std::map<std::string, float>& values, std::string& error) const;
    bool load(const std::string& presetName, std::map<std::string, float>& outValues,
              std::string& error) const;
    bool remove(const std::string& presetName) const;
    std::vector<std::string> list() const;

private:
    std::string dir_;
    std::string pathFor(const std::string& presetName) const;
};

} // namespace ce
