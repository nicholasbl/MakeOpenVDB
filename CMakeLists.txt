cmake_minimum_required(VERSION 3.1...3.16)

include(cmake/CPM.cmake)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/third_party/)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib/cmake/OpenVDB)
set(Boost_USE_STATIC_LIBS ON)

project(make_openvdb VERSION 1.0 LANGUAGES C CXX)

add_executable(make_openvdb src/main.cpp)
target_compile_features(make_openvdb PUBLIC cxx_std_17)

set(CMAKE_INCLUDE_CURRENT_DIR ON)


target_include_directories(make_openvdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/include/)
target_include_directories(make_openvdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_directories(make_openvdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib/)


set(ENABLE_VTK OFF CACHE BOOL "Enable the use of VTK file types")

target_sources(make_openvdb
    PRIVATE
        src/common.h
        src/vdb_tools.h
        src/binaryplugin.cpp
        src/binaryplugin.h
    )

if (${ENABLE_VTK})
    target_compile_definitions(make_openvdb PRIVATE -DENABLE_VTK)
    find_package(VTK COMPONENTS
      vtkIOXML
      vtkIOAMR
      vtkCommonCore
      vtkCommonDataModel
      vtkFiltersCore QUIET)

    target_include_directories(make_openvdb PRIVATE ${VTK_INCLUDE_DIR})
    target_link_libraries(make_openvdb PRIVATE ${VTK_LIBRARIES})

    target_sources(make_openvdb
        PRIVATE
            src/vtkplugin.cpp
            src/vtkplugin.h
    )
endif()

CPMAddPackage(
  NAME cxxopts
  GITHUB_REPOSITORY jarro2783/cxxopts
  GIT_TAG 5eca8a30012b69b76316b71fa391a89fe09256cb
  OPTIONS
    "CXXOPTS_BUILD_EXAMPLES Off"
    "CXXOPTS_BUILD_TESTS Off"
)

find_library(OPENVDB openvdb REQUIRED)
find_library(TBB tbb REQUIRED)
find_library(BLOSC blosc REQUIRED)
target_link_libraries(make_openvdb PRIVATE
    ${OPENVDB} ${TBB} ${BLOSC}
    boost_iostreams
)


set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
set (CMAKE_L_FLAGS_DEBUG "${CMAKE_L_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")

target_link_libraries(make_openvdb PRIVATE cxxopts)
