#ifndef BINARYPLUGIN_H
#define BINARYPLUGIN_H

#include "common.h"

#include <openvdb/openvdb.h>

class BinaryPlugin {
public:
    BinaryPlugin(Config const&);
    ~BinaryPlugin();

    static bool recognized(fs::path const&);

    openvdb::GridPtrVec convert(Config const&);
};

#endif // BINARYPLUGIN_H
