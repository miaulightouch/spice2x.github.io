#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <mutex>
#include <utility>

#include "external/tinyxml2/tinyxml2.h"

#include "game.h"

// settings
extern std::string CONFIG_PATH_OVERRIDE;

struct ConfigKeypadBindings {
    std::string keypads[2];
    std::filesystem::path card_paths[2];
};

struct ConfigOptionSnapshot {
    uint32_t revision;
    std::vector<Option> options;
};

enum class ConfigOptionUpdateResult {
    Success,
    Stale,
    SaveFailed,
};

class Config {
public:
    static Config &getInstance();
    bool getStatus();
    bool createConfigFile();
    uint32_t getRevision() const;

    bool addGame(Game &game, bool save = true);

    // flush pending in-memory changes to disk; intended to be paired with
    // addGame(game, false) in bulk-init paths so we write the XML once instead
    // of once per game
    bool save();

    bool updateBinding(const Game &game, const Button &button, int alternative);
    bool updateBinding(const Game &game, const Analog &analog);
    bool updateBinding(const Game &game, ConfigKeypadBindings &keypads);
    bool updateBinding(const Game &game, const Light &light, int alternative);
    bool updateBinding(const Game &game, const Option &option);

    std::vector<Button> getButtons(const std::string &game);
    std::vector<Button> getButtons(Game *);

    std::vector<Analog> getAnalogs(const std::string &game);
    std::vector<Analog> getAnalogs(Game *);

    ConfigKeypadBindings getKeypadBindings(const std::string &game);
    ConfigKeypadBindings getKeypadBindings(Game *);

    std::vector<Light> getLights(const std::string &game);
    std::vector<Light> getLights(Game *);

    std::vector<Option> getOptions(const std::string &game);
    std::vector<Option> getOptions(Game *);
    ConfigOptionSnapshot getOptionsSnapshot(const std::string &game);
    ConfigOptionUpdateResult updateOptions(
        const std::string &game,
        uint32_t expected_revision,
        const std::vector<std::pair<std::string, std::string>> &changes);

private:
    Config();
    Config(const Config &);
    ~Config() = default;

    const Config &operator=(const Config &);

    tinyxml2::XMLDocument configFile;
    bool status;
    std::filesystem::path configLocation;
    std::filesystem::path configLocationTemp;
    mutable std::recursive_mutex config_mutex;
    tinyxml2::XMLDocument savedConfigFile;
    uint32_t revision = 0;
    bool savedConfigValid = false;

    bool firstFillConfigFile();
    bool saveConfigFile();
};
