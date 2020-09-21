#include "common.h"

#ifdef ENABLE_VTK
#    include "vtkplugin.h"
#endif

#include "binaryplugin.h"


#include <openvdb/openvdb.h>
#include <openvdb/tools/Composite.h>

#include <array>
#include <cxxabi.h>
#include <iostream>
#include <queue>
#include <string_view>
#include <thread>


Config configure(std::queue<std::string_view> raw_args) {
    Config config;

    std::vector<std::string_view> args;

    while (!raw_args.empty()) {
        // take next
        auto str = raw_args.front();

        raw_args.pop();

        if (str.find("-") != 0) {
            args.push_back(str);
            continue;
        }

        // options better have another string after
        if (raw_args.empty()) break;

        auto next = raw_args.front();

        config.all_flags[std::string(str)] = std::string(next);
    }

    {
        // sort out options

        auto test_and_set = [&](std::string key, auto function) {
            auto iter = config.all_flags.find(key);

            if (iter != config.all_flags.end()) { function(iter->second); }
        };

        // what is the option?
        test_and_set("-o", [&](auto v) { config.output_path = v; });

        test_and_set("-p", [&](auto v) { config.requested_plugin = v; });

        test_and_set("-d", [&](auto v) { config.name_map[v] = "density"; });
        test_and_set("-t", [&](auto v) { config.name_map[v] = "temperature"; });
        test_and_set("-f", [&](auto v) { config.name_map[v] = "flame"; });

        test_and_set("-n", [&](auto v) { config.num_samples = std::stoi(v); });

        test_and_set("-d", [&](auto v) { config.sample_rate = std::stod(v); });
    }

    config.input_path = args.at(0);

    if (config.output_path.empty()) {
        config.output_path = config.input_path;
        config.output_path.replace_extension(".vdb");
    }

    if (config.num_samples and config.num_samples <= 0) {
        config.num_samples = 100;
    }

    if (config.sample_rate and config.sample_rate <= 0) {
        config.sample_rate = .1;
    }

    if (!config.num_samples and !config.sample_rate) {
        config.num_samples = 100;
    }

    std::cout << "Input file:  " << config.input_path << "\n";
    std::cout << "Output file: " << config.output_path << "\n";
    std::cout << "Mapping:\n";
    for (auto const& [k, v] : config.name_map) {
        std::cout << "\t" << k << " -> " << v << "\n";
    }

    return config;
}


using PluginHandler = std::function<bool(fs::path const&)>;

std::vector<PluginHandler>                             plugins;
std::unordered_map<std::string, std::function<void()>> plugin_map;

template <class T>
void install_plugin(openvdb::GridPtrVec& grids, Config const& config) {

    {
        size_t length = 0;
        int    status = 0;
        char*  type_name =
            abi::__cxa_demangle(typeid(T).name(), nullptr, &length, &status);

        plugin_map[type_name] = [&grids, &config]() {
            T p;
            grids = p.convert(config);
        };

        std::cout << "Registering " << type_name << "\n";

        free(type_name);
    }

    plugins.push_back([&grids, &config](fs::path const& ext) {
        if (!T::recognized(ext)) return false;

        T p;
        grids = p.convert(config);

        return true;
    });
}


int main(int argc, char* argv[]) {
    if (argc < 2) return EXIT_FAILURE;

    openvdb::initialize();

    auto const config = [&]() {
        std::queue<std::string_view> pack;
        for (int i = 1; i < argc; i++) {
            pack.push(argv[i]);
        }
        return configure(pack);
    }();


    std::cout << "Platform concurrency " << std::thread::hardware_concurrency()
              << "\n";

    if (!fs::exists(config.input_path)) return EXIT_FAILURE;

    std::cout << "Loading...\n";


    openvdb::GridPtrVec grids;

    auto ext = config.input_path.extension();

#ifdef ENABLE_VTK
    install_plugin<VTKPlugin>(grids, config);
#endif

    install_plugin<BinaryPlugin>(grids, config);

    if (config.requested_plugin.size()) {
        auto iter = plugin_map.find(config.requested_plugin);

        if (iter == plugin_map.end()) {
            std::cerr << "Unknown plugin requested!\n";
        }

        iter->second();
    } else {
        for (auto const& f : plugins) {
            if (f(ext)) break;
        }
    }


    openvdb::io::File file(config.output_path);
    file.write(grids);
    file.close();


    return 0;
}
