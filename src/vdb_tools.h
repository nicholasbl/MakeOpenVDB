#ifndef VDB_TOOLS_H
#define VDB_TOOLS_H

#include "common.h"

#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <type_traits>

#include <openvdb/openvdb.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/Prune.h>

template <typename... T>
struct dependent_false {
    static constexpr bool value = false;
};

template <class T>
struct Pair {
    T first, second;
};

template <class T>
Pair<T> make_pair(T a, T b) {
    return Pair<T> { a, b };
}

template <class Reader, class IterA>
auto vdb_chunk(Reader const& a,
               Config const& c,
               Pair<size_t>  xs,
               Pair<size_t>  ys,
               Pair<IterA>   zs) {
    auto sub_grid = openvdb::FloatGrid::create();
    auto accessor = sub_grid->getAccessor();

    openvdb::Coord ijk;

    int& x = ijk[0];
    int& y = ijk[1];
    int& z = ijk[2];

    using RetType = std::invoke_result_t<Reader, size_t, size_t, size_t>;

    for (z = zs.first; z != zs.second; ++z) {
        for (y = ys.first; y < ys.second; ++y) {
            for (x = xs.first; x < xs.second; ++x) {

                if constexpr (std::is_same_v<RetType, std::optional<float>>) {
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

        if (!c.use_threads && c.has_flag("--progress")) {
            std::cout << "P: " << z << "/" << zs.second - 1 << std::endl;
        }
    }
    return sub_grid;
}

template <class Reader>
[[nodiscard]] auto
build_open_vdb(std::array<size_t, 3> dims, Reader const& a, Config const& c) {
    std::cout << "Starting VDB build..." << std::endl;
    std::list<openvdb::FloatGrid::Ptr> sub_grids;

    std::mutex grid_mutex;


    if (c.use_threads) {
        tbb::parallel_for(
            tbb::blocked_range<int>(0, dims[2]),
            [dims, &a, &c, &grid_mutex, &sub_grids](auto const& range) {
                auto sub_grid =
                    vdb_chunk(a,
                              c,
                              { 0, dims[0] },
                              { 0, dims[1] },
                              make_pair(range.begin(), range.end()));

                {
                    std::scoped_lock lock(grid_mutex);

                    sub_grids.push_back(sub_grid);
                }
            });
    } else {
        auto grid = vdb_chunk(a,
                              c,
                              { 0, dims[0] },
                              { 0, dims[1] },
                              make_pair(size_t { 0 }, dims[2]));
        sub_grids.push_back(grid);
    }

    // merge grids

    std::cout << "Collecting VDB subgrids..." << std::endl;

    openvdb::FloatGrid::Ptr main_grid;

    if (sub_grids.size() == 0) { return main_grid; }

    if (sub_grids.size() == 1) {
        main_grid = sub_grids.back();
    } else {
        main_grid = openvdb::FloatGrid::create();

        while (!sub_grids.empty()) {
            auto ptr = sub_grids.back();
            sub_grids.pop_back();

            openvdb::tools::compReplace(*main_grid, *ptr);
        }
    }

    if (c.prune_amount) {
        std::cout << "Pruning..." << std::endl;
        openvdb::tools::prune(main_grid->tree(), *c.prune_amount);
    }

    return main_grid;
}


#endif // VDB_TOOLS_H
