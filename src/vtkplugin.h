#ifndef VTKPLUGIN_H
#define VTKPLUGIN_H

#include "common.h"

#include <openvdb/openvdb.h>

class VTKPlugin {
public:
    VTKPlugin(Config const&);
    ~VTKPlugin();

    static bool recognized(fs::path const&);

    openvdb::GridPtrVec convert(Config const&);
};

#endif // VTKPLUGIN_H
