cmake_minimum_required(VERSION 3.1...3.16)

project(make_openvdb VERSION 1.0 LANGUAGES CXX)

add_executable(make_openvdb main.cpp)
target_compile_features(make_openvdb PUBLIC cxx_std_17)

target_include_directories(make_openvdb PUBLIC /usr/local/include)

set(ENABLE_VTK OFF CACHE BOOL "Enable the use of VTK file types")

target_sources(make_openvdb
    PRIVATE
        common.h
        vdb_tools.h
        binaryplugin.cpp
        binaryplugin.h
    )

if (${ENABLE_VTK})
    target_compile_definitions(make_openvdb PRIVATE -DENABLE_VTK)
    find_package(VTK COMPONENTS
      vtkIOXML
      vtkCommonCore
      vtkCommonDataModel
      vtkFiltersCore QUIET)

    target_include_directories(make_openvdb PRIVATE ${VTK_INCLUDE_DIR})
    target_link_libraries(make_openvdb PRIVATE ${VTK_LIBRARIES})

    target_sources(make_openvdb
        PRIVATE
            vtkplugin.cpp
            vtkplugin.h
    )
endif()


# These are required
find_library(OPENVDB openvdb REQUIRED)
find_library(TBB tbb REQUIRED)
find_library(HALF half REQUIRED)

find_path(OPENVDB_INC openvdb/openvdb.h REQUIRED)

set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")


target_include_directories(make_openvdb PRIVATE ${OPENVDB_INC})
target_link_libraries(make_openvdb PRIVATE ${OPENVDB} ${TBB} ${HALF})

