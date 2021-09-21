#ifndef COMMON_H
#define COMMON_H

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

struct Config {
    std::string requested_plugin;

    fs::path input_path;
    fs::path output_path;

    std::unordered_map<std::string, std::string> all_flags;

    std::unordered_map<std::string, std::string> name_map;

    std::optional<int>    num_samples;
    std::optional<double> sample_rate;

    std::optional<int> requested_amr_level;

    bool use_threads = true;

    std::optional<float> prune_amount;

    std::optional<std::string> bin_dims;

    std::string get_flag(std::string key) const {
        auto iter = all_flags.find(key);
        if (iter == all_flags.end()) return {};
        return iter->second;
    }

    bool has_flag(std::string key) const { return all_flags.count(key); }
};


#endif // COMMON_H
