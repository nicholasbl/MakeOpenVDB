#include "vtkplugin.h"

#include "vdb_tools.h"

#include <vtkAMRInformation.h>
#include <vtkAMReXGridReader.h>
#include <vtkCompositeDataIterator.h>
#include <vtkCompositeDataSet.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkMultiProcessController.h>
#include <vtkOverlappingAMR.h>
#include <vtkPointData.h>
#include <vtkResampleToImage.h>
#include <vtkSMPTools.h>
#include <vtkSmartPointer.h>
#include <vtkUnstructuredGrid.h>
#include <vtkXMLImageDataReader.h>
#include <vtkXMLMultiBlockDataReader.h>


#include <openvdb/openvdb.h>
#include <openvdb/tools/Composite.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <queue>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

enum class VTKTYPE {
    UNKNOWN,
    CHAR,
    S_CHAR,
    U_CHAR,
    FLOAT,
    DOUBLE,
};

VTKTYPE figure_type(int t) {
    // std::cerr << "UT:" << t << std::endl;
    switch (t) {
    case VTK_CHAR: return VTKTYPE::CHAR;
    case VTK_SIGNED_CHAR: return VTKTYPE::S_CHAR;
    case VTK_UNSIGNED_CHAR: return VTKTYPE::U_CHAR;
    case VTK_FLOAT: return VTKTYPE::FLOAT;
    case VTK_DOUBLE: return VTKTYPE::DOUBLE;
    }

    return VTKTYPE::UNKNOWN;
}

auto write_to_grid(vtkDataArray*                array,
                   std::string const&           override_name,
                   std::array<size_t, 3> const& dims) {

    std::cout << "Working on: " << array->GetName() << "\n";

    int num_comp = array->GetNumberOfComponents();

    std::cout << "Number of components " << num_comp << std::endl;

    auto type = figure_type(array->GetDataType());

    std::cout << "Type: " << static_cast<int>(type) << std::endl;

    auto range_min = array->GetRange()[0];
    auto range_max = array->GetRange()[1];

    std::cout << "Range: " << range_min << " " << range_max << std::endl;

    auto main_grid = build_open_vdb(
        dims,
        [&dims, &array, range_min](
            size_t x, size_t y, size_t z) -> std::optional<float> {
            int idx = x + y * dims[0] + z * dims[0] * dims[1];

            double cache[4];

            array->GetTuple(idx, cache);

            double value = cache[0];

            if (value > range_min) return value;

            return std::nullopt;
        });


    if (override_name.size()) {
        main_grid->setName(override_name);

        main_grid->insertMeta("source_name",
                              openvdb::StringMetadata(override_name));

    } else {
        main_grid->setName(array->GetName());
    }


    /*

    std::list<openvdb::FloatGrid::Ptr> sub_grids;

    std::mutex grid_mutex;

    tbb::parallel_for(
        tbb::blocked_range<int>(0, dims[2]),
        [dims, range_min, &array, &grid_mutex, &sub_grids](auto const&
    range) { auto sub_grid = openvdb::FloatGrid::create(); auto accessor =
    sub_grid->getAccessor();

            openvdb::Coord ijk;

            int& x = ijk[0];
            int& y = ijk[1];
            int& z = ijk[2];

            for (z = range.begin(); z != range.end(); ++z) {
                for (y = 0; y < dims[1]; ++y) {
                    for (x = 0; x < dims[0]; ++x) {
                        int idx = x + y * dims[0] + z * dims[0] * dims[1];

                        double cache[4];

                        array->GetTuple(idx, cache);

                        double value = cache[0];

                        if (value > range_min)
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

    */


    return main_grid;
}

openvdb::GridPtrVec convert_image(vtkImageData* image, Config const& config) {
    openvdb::GridPtrVec ret;

    std::array<size_t, 3> dims;

    {
        std::array<int, 3> int_dims;
        image->GetDimensions(int_dims.data());

        for (size_t i = 0; i < dims.size(); i++) {
            if (int_dims[i] <= 0) {
                throw std::runtime_error(
                    "Negative or zero dimensions from vtk.");
            }

            dims[i] = int_dims[i];
        }
    }

    std::cout << "Converting Image " << dims[0] << " " << dims[1] << " "
              << dims[2] << "\n";

    auto* point_data = image->GetPointData();

    int num_arrays = point_data->GetNumberOfArrays();

    std::cout << "Converting...\n";

    for (int i = 0; i < num_arrays; i++) {
        auto* array = point_data->GetArray(i);
        if (!array) continue;

        auto iter = config.name_map.find(array->GetName());

        if (iter == config.name_map.end()) continue;

        auto ptr = write_to_grid(array, iter->second, dims);
        ret.push_back(ptr);
    }

    return ret;
}

openvdb::GridPtrVec convert_vti(Config const& config) {
    openvdb::GridPtrVec ret;

    auto reader = vtkSmartPointer<vtkXMLImageDataReader>::New();

    reader->SetFileName(config.input_path.c_str());
    reader->Update();

    auto* im = reader->GetOutput();

    return convert_image(im, config);
}

std::array<int, 3> compute_sample_rate(Config const& config, double bounds[6]) {
    std::array<int, 3> num_samples;

    // sample rate is the max number of samples on a side.
    // try to keep sampling isotropic

    double deltas[3] = { bounds[1] - bounds[0],
                         bounds[3] - bounds[2],
                         bounds[5] - bounds[4] };

    std::cout << "Deltas " << deltas[0] << " " << deltas[1] << " " << deltas[2]
              << "\n";


    double min_size = std::min(deltas[0], std::min(deltas[1], deltas[2]));

    double scales[3] = { deltas[0] / min_size,
                         deltas[1] / min_size,
                         deltas[2] / min_size };

    double factor = 1;

    if (config.num_samples) {
        factor = config.num_samples.value();
    } else if (config.sample_rate) {
        factor = min_size / config.sample_rate.value();
    } else {
        factor = 100;
    }

    num_samples[0] = scales[0] * factor;
    num_samples[1] = scales[1] * factor;
    num_samples[2] = scales[2] * factor;

    return num_samples;
}

openvdb::GridPtrVec convert_vtm(Config const& config) {
    openvdb::GridPtrVec ret;

    auto reader = vtkSmartPointer<vtkXMLMultiBlockDataReader>::New();

    reader->SetFileName(config.input_path.c_str());
    reader->Update();

    auto* composite = reader->GetOutput();

    auto* mblocks = vtkMultiBlockDataSet::SafeDownCast(composite);

    if (!mblocks) return ret;

    double bounds[6];

    {
        auto* iter = mblocks->NewIterator();

        while (!iter->IsDoneWithTraversal()) {

            auto* obj = iter->GetCurrentDataObject();

            auto* usgrid = vtkUnstructuredGrid::SafeDownCast(obj);

            if (usgrid) {
                usgrid->GetBounds(bounds);

                // for now
                break;
            }


            iter->GoToNextItem();
        }

        iter->Delete();
    }

    std::cout << "Bounds " << bounds[0] << " " << bounds[1] << " " << bounds[2]
              << " " << bounds[3] << " " << bounds[4] << " " << bounds[5]
              << "\n";

    std::array<int, 3> num_samples = compute_sample_rate(config, bounds);

    std::cout << "Sampling " << num_samples[0] << " " << num_samples[1] << " "
              << num_samples[2] << "\n";


    for (unsigned block_iter = 0; block_iter < mblocks->GetNumberOfBlocks();
         block_iter++) {

        auto sampler = vtkSmartPointer<vtkResampleToImage>::New();

        sampler->SetInputDataObject(mblocks->GetBlock(block_iter));
        sampler->SetUseInputBounds(true);
        sampler->SetSamplingDimensions(
            num_samples[0], num_samples[1], num_samples[2]);


        sampler->Update();

        auto sub_parts = convert_image(sampler->GetOutput(), config);

        ret.insert(ret.end(), sub_parts.begin(), sub_parts.end());
    }

    return ret;
}

// amr.n_cell
// amr.n_cell = 16 16 32

// Taken from Dacite source
enum class StringSplitControl {
    KEEP_EMPTY_PARTS,
    SKIP_EMPTY_PARTS,
};

template <class Function>
void split(std::string_view   str,
           std::string_view   delim,
           StringSplitControl c,
           Function&&         output_fun) {

    auto       first  = str.begin();
    auto       second = str.begin();
    auto const last   = str.end();

    for (; second != last and first != last; first = second + 1) {
        assert(first - str.data() < str.size());
        assert(second - str.data() < str.size());
        second = std::find_first_of(first, last, delim.cbegin(), delim.cend());

        second = (second == (const char*)std::string::npos) ? last : second;

        assert(second - str.data() <= str.size());

        if (first != second or c == StringSplitControl::KEEP_EMPTY_PARTS) {
            auto sp = std::string_view(first, second - first);
            output_fun(sp);
        }
    }
}

std::array<int, 3> find_cell_counts(Config const&      config,
                                    vtkOverlappingAMR* output) {
    // TODO find if there is VTK way of getting this info...I can't seem to find
    // any.

    std::cout << "Finding cell counts...\n";

    if (!fs::is_directory(config.input_path)) {
        std::cerr << "Input path is not a dir??\n";
        return {};
    }


    for (auto const& dir_entry :
         std::filesystem::directory_iterator { config.input_path }) {

        if (!fs::is_regular_file(dir_entry)) continue;

        std::cout << dir_entry.path() << '\n';

        std::ifstream ifs(dir_entry.path());

        if (!ifs.is_open()) continue;


        std::string line;

        while (std::getline(ifs, line)) {
            if (line.empty()) continue;

            if (line.rfind("amr.n_cell", 0) != 0) continue;

            auto eq_pos = line.find('=');

            if (eq_pos == std::string::npos) continue;

            auto l = std::string_view(line).substr(eq_pos + 1);

            if (l.empty()) continue;

            std::array<int, 3> ret = { 1, 1, 1 };

            size_t cursor = 0;

            split(l, " ", StringSplitControl::SKIP_EMPTY_PARTS, [&](auto v) {
                ret.at(cursor) = std::stoi(std::string(v));
                cursor++;
            });

            std::cout << "Found:" << ret[0] << " " << ret[1] << " " << ret[2]
                      << std::endl;

            return ret;
        }
    }

    // if we got here, we havent found a cell count. we should try to figure it
    // out.

    auto ncells = output->GetNumberOfCells();

    double bounds[6] = { 0, 0, 0, 0, 0, 0 };
    output->GetBounds(bounds);

    double deltas[3] = { bounds[1] - bounds[0],
                         bounds[3] - bounds[2],
                         bounds[5] - bounds[4] };

    auto dsum = std::sqrt(deltas[0] * deltas[0] + deltas[1] * deltas[1] +
                          deltas[2] * deltas[2]);

    std::array<int, 3> ret;

    for (int i = 0; i < 3; i++) {
        ret[i] = std::ceil(deltas[i] * ncells / dsum);
    }

    return ret;
}

openvdb::GridPtrVec convert_amrex(Config const& config) {
    openvdb::GridPtrVec ret;

    auto reader = vtkSmartPointer<vtkAMReXGridReader>::New();

    reader->SetFileName(config.input_path.c_str());
    reader->Update();

    const int level =
        config.requested_amr_level.value_or(reader->GetNumberOfLevels());

    reader->SetMaxLevel(level);
    reader->Update();

    for (int i = 0; i < reader->GetNumberOfCellArrays(); i++) {
        auto name = reader->GetCellArrayName(i);

        if (config.name_map.count(name)) {
            reader->SetCellArrayStatus(name, 1);
        }
    }

    reader->Update();

    auto* output = reader->GetOutput();


    std::array<int, 3> extents = find_cell_counts(config, output);


    int refinement_ratio =
        output->GetRefinementRatio(reader->GetNumberOfLevels() - 1); //?

    std::cout << "Levels: " << level << std::endl;
    std::cout << "Ratio:  " << refinement_ratio << std::endl;

    double bounds[6] = { 0, 0, 0, 0, 0, 0 };
    output->GetBounds(bounds);

    // output->GetAMRInfo()->GetSpacing(level, extents);


    std::cout << "Bounds " << bounds[0] << " " << bounds[1] << " " << bounds[2]
              << " " << bounds[3] << " " << bounds[4] << " " << bounds[5]
              << "\n";

    std::cout << "Extents " << extents[0] << " " << extents[1] << " "
              << extents[2] << "\n";


    std::array<int, 3> num_samples;

    if (config.sample_rate or config.num_samples) {
        num_samples = compute_sample_rate(config, bounds);
    } else {
        auto scale_by = std::pow(refinement_ratio, level);

        std::cout << "Scale By: " << scale_by << std::endl;

        for (int i = 0; i < 3; i++) {
            num_samples[i] = extents[i] * scale_by;
        }
    }

    std::cout << "Sampling " << num_samples[0] << " " << num_samples[1] << " "
              << num_samples[2] << "\n";

    auto sampler = vtkSmartPointer<vtkResampleToImage>::New();

    sampler->SetInputDataObject(output);
    sampler->SetUseInputBounds(true);
    sampler->SetSamplingDimensions(
        num_samples[0], num_samples[1], num_samples[2]);


    sampler->Update();

    auto sub_parts = convert_image(sampler->GetOutput(), config);

    ret.insert(ret.end(), sub_parts.begin(), sub_parts.end());

    return ret;
}


VTKPlugin::VTKPlugin(Config const& config) {
    auto hwc = config.num_threads.value_or(std::thread::hardware_concurrency());
    vtkSMPTools::Initialize(hwc);

    std::cout << "VTK Concurrency: "
              << vtkSMPTools::GetEstimatedNumberOfThreads() << "\n";
}

VTKPlugin::~VTKPlugin() { }

bool VTKPlugin::recognized(fs::path const& exts) {
    if (exts == ".vti") { return true; }
    if (exts == ".vtm") { return true; }

    return false;
}

openvdb::GridPtrVec VTKPlugin::convert(Config const& config) {
    auto ext = config.input_path.extension();

    if (ext == ".vti") {
        return convert_vti(config);
    } else if (ext == ".vtm") {
        return convert_vtm(config);
    }

    // has the user given us a type hint for something else?
    auto is_amrex = config.all_flags.count("amrex");

    if (is_amrex) { return convert_amrex(config); }

    return {};
}
