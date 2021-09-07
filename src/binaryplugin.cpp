#include "binaryplugin.h"

#include "vdb_tools.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <charconv>
#include <fstream>

BinaryPlugin::BinaryPlugin(Config const&) { }

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
    if (!c.bin_dims) {
        std::cerr << "Need flat binary file dimensions flag (--dims X:Y:Z).\n";
        return std::nullopt;
    }

    std::string_view view = c.bin_dims.value();

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

// cant use span due to no support < gcc 10
template <class T>
std::tuple<T const*, size_t, int> map_file_to(fs::path const& file) {

    if (!fs::is_regular_file(file)) return { nullptr, 0, 0 };

    auto file_size = fs::file_size(file);

    int fd = open(file.c_str(), O_RDONLY);

    if (fd < 0) {
        // badness
        return { nullptr, 0, 0 };
    }

    void* ptr =
        mmap(nullptr, file_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);

    if (!ptr) {
        close(fd);
        return { nullptr, 0, 0 };
    }

    size_t element_count = file_size / sizeof(T);

    return { reinterpret_cast<T const*>(ptr), element_count, fd };
}

inline size_t
compute_index(size_t x, size_t y, size_t z, std::array<size_t, 3> const& dims) {
    // return x + dims[0] * (y + dims[1] * z);
    return z + dims[2] * (y + dims[1] * x);
}

template <class T>
auto consume_mapping(std::array<size_t, 3>             dims,
                     std::tuple<T const*, size_t, int> mapping,
                     std::string                       name,
                     Config const&                     c) {

    auto [data, element_count, fd] = mapping;

    // workaround for noncapture of structured bindings
    auto handler = [data = data, element_count = element_count, dims](
                       size_t x, size_t y, size_t z) -> float {
        auto index = compute_index(x, y, z, dims);

        assert(index < element_count);

        return float(data[index]);
    };

    auto grid = build_open_vdb(dims, handler, c);

    grid->setName(name);

    munmap((void*)(data), element_count * sizeof(T));

    close(fd);

    return grid;
}

openvdb::GridPtrVec BinaryPlugin::convert(Config const& c) {
    openvdb::GridPtrVec ret;

    auto result = get_dims(c);

    if (!result.has_value()) return ret;

    bool is_double = false;

    if (c.has_flag("--bin_double")) {
        std::cout << "Using doubles..." << std::endl;
        is_double = true;
    }

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
        ret.push_back(consume_mapping(
            dims, map_file_to<double>(c.input_path), data_name, c));

    } else {
        ret.push_back(consume_mapping(
            dims, map_file_to<float>(c.input_path), data_name, c));
    }

    return ret;
}
