#ifndef VDB_TOOLS_H
#define VDB_TOOLS_H

#include <optional>
#include <type_traits>

#include <openvdb/openvdb.h>
#include <openvdb/tools/Composite.h>

template <typename... T>
struct dependent_false {
    static constexpr bool value = false;
};


template <class Reader>
[[nodiscard]] auto build_open_vdb(std::array<size_t, 3> dims, Reader const& a) {
    auto main_grid = openvdb::FloatGrid::create();

    std::list<openvdb::FloatGrid::Ptr> sub_grids;

    std::mutex grid_mutex;


    tbb::parallel_for(
        tbb::blocked_range<int>(0, dims[2]),
        [dims, &a, &grid_mutex, &sub_grids](auto const& range) {
            auto sub_grid = openvdb::FloatGrid::create();
            auto accessor = sub_grid->getAccessor();

            openvdb::Coord ijk;

            int& x = ijk[0];
            int& y = ijk[1];
            int& z = ijk[2];

            using RetType =
                std::invoke_result_t<Reader, size_t, size_t, size_t>;

            for (z = range.begin(); z != range.end(); ++z) {
                for (y = 0; y < dims[1]; ++y) {
                    for (x = 0; x < dims[0]; ++x) {

                        if constexpr (std::is_same_v<RetType,
                                                     std::optional<float>>) {
                            auto value = a(x, y, z);

                            if (value.has_value()) {
                                accessor.setValue(ijk, value.value());
                            }
                        } else if constexpr (std::is_same_v<RetType, float>) {

                            auto value = a(x, y, z);

                            accessor.setValue(ijk, value);

                        } else {
                            static_assert(dependent_false<RetType>::value,
                                          "Unknown Reader return type");
                        }
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

    return main_grid;
}


#endif // VDB_TOOLS_H
