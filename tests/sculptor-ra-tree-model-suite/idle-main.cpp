#include "mesh-nic.h"

// Unused physical mesh positions still need an ELF image.  This guest joins
// memory initialization, then exits without participating in the deployment.
extern "C" int tile_main() {
    mesh_nic::complete_memory_initialization();
    return 0;
}
