#include "binaryplugin.h"

#include "vdb_tools.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <charconv>
#include <fstream>

// cant use span due to no support < gcc 10
// laziness abounds in this code...

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

struct MemData {
    std::unique_ptr<std::byte[]> data;
    std::size_t                  byte_count;

    std::byte const* begin() const { return data.get(); }
};

struct MapData {
    int fd = -1;

    std::byte const* data;
    size_t           byte_count;

    ~MapData() {
        if (fd >= 0) {
            munmap((void*)(data), byte_count);
            close(fd);
        }
    }

    std::byte const* begin() const { return data; }
};

std::unique_ptr<MemData> read_file_into(fs::path const& file) {

    if (!fs::is_regular_file(file)) return nullptr;

    auto file_size = fs::file_size(file);

    std::ifstream ifs(file, std::ios::in | std::ios::binary);

    if (!ifs.good()) return nullptr;

    auto ret = std::make_unique<MemData>();

    ret->data = std::make_unique<std::byte[]>(file_size);

    ifs.read(reinterpret_cast<char*>(ret->data.get()), file_size);

    if (!ifs) { return nullptr; }

    ret->byte_count = file_size;

    return ret;
}


std::unique_ptr<MapData> map_file_to(fs::path const& file) {

    if (!fs::is_regular_file(file)) return nullptr;

    auto file_size = fs::file_size(file);

    int fd = open(file.c_str(), O_RDONLY);

    if (fd < 0) {
        // badness
        return nullptr;
    }

    void* ptr =
        mmap(nullptr, file_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);

    if (!ptr) {
        close(fd);
        return nullptr;
    }

    auto ret        = std::make_unique<MapData>();
    ret->fd         = fd;
    ret->data       = reinterpret_cast<std::byte const*>(ptr);
    ret->byte_count = file_size;

    return ret;
}

inline size_t
compute_index(size_t x, size_t y, size_t z, std::array<size_t, 3> const& dims) {
    // return x + dims[0] * (y + dims[1] * z);
    return z + dims[2] * (y + dims[1] * x);
}

template <class T, class S>
auto consume_mapping(std::array<size_t, 3> dims,
                     S const&              source,
                     std::string           name,
                     Config const&         c) {

    auto src_ptr    = source.begin();
    auto byte_count = source.byte_count;

    size_t element_count = byte_count / sizeof(T);
    auto   data          = reinterpret_cast<T const*>(src_ptr);

    auto handler = [=](size_t x, size_t y, size_t z) -> float {
        auto index = compute_index(x, y, z, dims);

        assert(index < element_count);

        return float(data[index]);
    };

    auto grid = build_open_vdb(dims, handler, c);

    grid->setName(name);

    return grid;
}

template <class S>
auto process_with(std::array<size_t, 3> dims,
                  S const&              source,
                  std::string           name,
                  Config const&         c,
                  bool                  is_double) {
    if (is_double) {
        return consume_mapping<double>(dims, source, name, c);
    } else {
        return consume_mapping<float>(dims, source, name, c);
    }
}

template <class F>
auto convert_binary(std::array<size_t, 3> dims,
                    std::string           name,
                    Config const&         c,
                    bool                  is_double,
                    F&&                   handler) {

    auto r = handler(c.input_path);

    if (!r) throw std::runtime_error("Unable to read file");

    return process_with(dims, *r, name, c, is_double);
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

    bool use_memmap = false;

    if (c.has_flag("--bin_memmap")) {
        std::cout << "Using memory mapping..." << std::endl;
        use_memmap = true;
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


    if (use_memmap) {
        ret.push_back(
            convert_binary(dims, data_name, c, is_double, map_file_to));

    } else {
        ret.push_back(
            convert_binary(dims, data_name, c, is_double, read_file_into));
    }

    return ret;
}
