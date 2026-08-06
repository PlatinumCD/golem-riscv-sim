#include "mesh-nic.h"
#include "platform.h"

extern "C" int tile_main() {
    mesh_nic::complete_memory_initialization();
    return 0;
}
