#include "common.h"

#ifdef ENABLE_VTK
#    include "vtkplugin.h"
#endif

#include "binaryplugin.h"

#include <cxxopts.hpp>

#include <openvdb/openvdb.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/Prune.h>

#include <array>
#include <cxxabi.h>
#include <iostream>
#include <queue>
#include <string_view>
#include <thread>


template <class T, class Function>
auto test_and_set(cxxopts::ParseResult const& result,
                  std::string                 opt,
                  Function&&                  function) {
    try {
        function(result[opt].as<T>());
    } catch (...) { }
}

Config configure(cxxopts::ParseResult& result) {
    Config config;

    test_and_set<std::string>(result, "plugin", [&](auto v) {
        if (v != "auto") config.requested_plugin = v;
    });

    test_and_set<std::string>(result, "density", [&](auto v) {
        if (!v.empty()) config.name_map[v] = "density";
    });
    test_and_set<std::string>(result, "temp", [&](auto v) {
        if (!v.empty()) config.name_map[v] = "temperature";
    });
    test_and_set<std::string>(result, "flame", [&](auto v) {
        if (!v.empty()) config.name_map[v] = "flame";
    });

    test_and_set<int>(result, "nsample", [&](auto v) {
        if (v > 0) config.num_samples = v;
    });

    test_and_set<float>(result, "rate", [&](auto v) {
        if (v > 0) config.sample_rate = v;
    });

    test_and_set<int>(result, "level", [&](auto v) {
        if (v > 0) {
            std::cout << "Using level: " << v << std::endl;
            config.requested_amr_level = v;
        }
    });

    test_and_set<int>(result, "threads", [&](auto v) {
        if (v > 0) {
            std::cout << "Using threads: " << v << std::endl;
            config.num_threads = v;
        }
    });

    test_and_set<bool>(result, "prune", [&](auto v) {
        if (v) {
            std::cout << "Enable prune." << std::endl;
            config.prune_amount = 0;
        }
    });

    test_and_set<float>(result, "prune_amount", [&](auto v) {
        if (v < 0) return;
        std::cout << "Prune: " << v << std::endl;
        config.prune_amount = v;
    });

    test_and_set<std::string>(result, "bin_dims", [&](auto v) {
        if (v.empty()) return;
        std::cout << "Bin Dims: " << v << std::endl;
        config.bin_dims = v;
    });


    config.input_path  = result["input"].as<std::string>();
    config.output_path = result["output"].as<std::string>();

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

    for (auto const& kv : result.unmatched()) {
        config.all_flags[kv] = std::string("1");
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
            T p(config);
            grids = p.convert(config);
        };

        std::cout << "Registering " << type_name << "\n";

        free(type_name);
    }

    plugins.push_back([&grids, &config](fs::path const& ext) {
        if (!T::recognized(ext)) return false;

        T p(config);
        grids = p.convert(config);

        return true;
    });
}


int main(int argc, char* argv[]) {
    openvdb::initialize();

    cxxopts::Options options("make_openvdb",
                             "Convert files to a blender-friendly openvdb");

    // clang-format off
    options.add_options()
            ("p,plugin",
             "Request plugin",
             cxxopts::value<std::string>()->default_value("auto"))
            ("d,density",
             "Map name to density",
             cxxopts::value<std::string>()->default_value(""))
            ("t,temp",
             "Map name to temperature",
             cxxopts::value<std::string>()->default_value(""))
            ("f,flame",
             "Map name to flame",
             cxxopts::value<std::string>()->default_value(""))
            ("n,nsample",
             "Override sampling with a number of samples",
             cxxopts::value<int>()->default_value("0"))
            ("r,rate",
             "Override sampling with a rate",
             cxxopts::value<float>()->default_value("-1.0"))
            ("l,level",
             "Requested AMR Level",
             cxxopts::value<int>()->default_value("-1"))
            ("threads",
             "Maximum threads to use",
             cxxopts::value<int>()->default_value("-1"))
            ("prune",
             "Permit pruning",
             cxxopts::value<bool>()->default_value("false"))
            ("prune_amount",
             "Set pruning tolerance",
             cxxopts::value<float>()->default_value("-1"))
            ("bin_dims",
             "Set binary volume dimensions",
             cxxopts::value<std::string>()->default_value(""))
            ("i,input", "Input file", cxxopts::value<std::string>())
            ("o,output", "Output file", cxxopts::value<std::string>())
            ("positional",
            "Positional arguments: these are the arguments that are entered "
            "without an option",
             cxxopts::value<std::vector<std::string>>()
             )
            ;
    // clang-format on

    options.allow_unrecognised_options();

    options.parse_positional({ "input", "output" });


    auto result = options.parse(argc, argv);


    auto const config = configure(result);


    std::cout << "Platform concurrency " << std::thread::hardware_concurrency()
              << "\n";


    if (!fs::is_regular_file(config.input_path)) {
        if (!fs::is_directory(config.input_path)) {
            std::cerr << "Unable to open input file!\n";
            return EXIT_FAILURE;
        }
    }

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
