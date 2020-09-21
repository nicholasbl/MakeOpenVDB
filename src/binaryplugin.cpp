#include "binaryplugin.h"

#include "vdb_tools.h"

#include <charconv>
#include <fstream>

BinaryPlugin::BinaryPlugin() { }

BinaryPlugin::~BinaryPlugin() { }

bool BinaryPlugin::recognized(fs::path const& exts) {
    if (exts == ".bin") { return true; }
    return false;
}

void split_ref_into(std::string_view               str,
                    std::string_view               delim,
                    std::vector<std::string_view>& output) {
    output.clear();

    auto first  = str.data();
    auto second = str.data();
    auto last   = first + str.size();

    for (; second != last && first != last; first = second + 1) {
        second = std::find_first_of(first, last, delim.cbegin(), delim.cend());

        output.emplace_back(first, second - first);
    }
}

std::optional<std::array<size_t, 3>> get_dims(Config const& c) {
    auto dim_string = c.get_flag("--dims");

    if (dim_string.empty()) {
        std::cerr << "Need flat binary file dimensions flag (--dims X:Y:Z).\n";
        return std::nullopt;
    }

    std::string_view view = dim_string;

    std::vector<std::string_view> splits;

    split_ref_into(view, ":", splits);

    std::array<size_t, 3> ret = { 1, 1, 1 };

    size_t bound = std::min(splits.size(), ret.size());

    for (size_t i = 0; i < bound; i++) {
        auto this_split = splits[i];

        int64_t value = 0;

        auto result =
            std::from_chars(this_split.begin(), this_split.end(), value);

        if (result.ec != std::errc()) {
            std::cerr << "Unable to read dimensions, check format.";
            return std::nullopt;
        }

        if (value <= 0) {
            std::cerr << "Dimensions must be >= 0.";
            return std::nullopt;
        }

        ret[i] = value;
    }

    return ret;
}

template <class T>
bool read_file_into(fs::path const& file, std::vector<T>& v) {
    std::ifstream strm(file.c_str(),
                       std::ios::in | std::ios::binary | std::ios::ate);

    if (!strm.good()) {
        std::cerr << "Unable to open file.\n";
        return false;
    }

    size_t file_size = strm.tellg();

    auto needed_byte_count = sizeof(T) * v.size();

    if (file_size < needed_byte_count) {
        std::cerr << "File not large enough; only " << file_size
                  << " bytes available.\n";
        return false;
    }

    strm.seekg(0, std::ios::beg);

    auto* start = reinterpret_cast<char*>(v.data());

    strm.read(start, needed_byte_count);

    if (!strm.good()) {
        std::cerr << "Unable to read all file contents.\n";
        return false;
    }

    return true;
}

inline size_t
compute_index(size_t x, size_t y, size_t z, std::array<size_t, 3> const& dims) {
    return x + dims[0] * (y + dims[1] * z);
}

template <class T>
auto consume_vector(std::array<size_t, 3> dims,
                    std::vector<T> const& v,
                    std::string           name) {

    auto grid = build_open_vdb(
        dims, [&v, &dims](size_t x, size_t y, size_t z) -> float {
            auto index = compute_index(x, y, z, dims);

            return float(v[index]);
        });

    grid->setName(name);

    return grid;

    /*

    auto main_grid = openvdb::FloatGrid::create();

    main_grid->setName(name);


    std::list<openvdb::FloatGrid::Ptr> sub_grids;

    std::mutex grid_mutex;


    tbb::parallel_for(tbb::blocked_range<int>(0, dims[2]),
                      [dims, &v, &grid_mutex, &sub_grids](auto const& range) {
                          auto sub_grid = openvdb::FloatGrid::create();
                          auto accessor = sub_grid->getAccessor();

                          openvdb::Coord ijk;

                          int& x = ijk[0];
                          int& y = ijk[1];
                          int& z = ijk[2];

                          for (z = range.begin(); z != range.end(); ++z) {
                              for (y = 0; y < dims[1]; ++y) {
                                  for (x = 0; x < dims[0]; ++x) {

                                      auto index = compute_index(x, y, z, dims);

                                      T value = v[index];

                                      accessor.setValue(ijk, float(value));
                                  }
                              }
                          }

                          {
                              std::scoped_lock lock(grid_mutex);

                              sub_grids.push_back(sub_grid);
                          }
                      });

    {
        // merge grids

        while (!sub_grids.empty()) {
            auto ptr = sub_grids.back();
            sub_grids.pop_back();

            openvdb::tools::compReplace(*main_grid, *ptr);
        }
    }

    return main_grid;*/
}

openvdb::GridPtrVec BinaryPlugin::convert(Config const& c) {
    openvdb::GridPtrVec ret;

    auto result = get_dims(c);

    if (!result.has_value()) return ret;

    bool is_double = false;

    auto type_string = c.get_flag("--type");

    if (type_string == "double") { is_double = true; }

    auto dims = result.value();

    size_t total_element_count = dims[0] * dims[1] * dims[2];

    size_t byte_count = total_element_count;

    if (is_double) {
        byte_count *= sizeof(double);
    } else {
        byte_count *= sizeof(float);
    }

    std::cout << "Reading " << byte_count << " bytes...\n";

    auto data_name = c.input_path.stem().string();

    { // remap name
        auto iter = c.name_map.find(data_name);

        if (iter != c.name_map.end()) { data_name = iter->second; }
    }

    std::cout << "Storing data in field: " << data_name << std::endl;

    if (is_double) {
        std::vector<double> raw_data(total_element_count);

        read_file_into(c.input_path, raw_data);

        ret.push_back(consume_vector(dims, raw_data, data_name));

    } else {
        std::vector<float> raw_data(total_element_count);

        read_file_into(c.input_path, raw_data);
    }

    return ret;
}
