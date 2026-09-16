#ifndef SST_MITTENS_GLOBAL_RAM_BACKING_H
#define SST_MITTENS_GLOBAL_RAM_BACKING_H

#include <cstdint>
#include <string>

namespace SST {
namespace Mittens {

// Returns a close-on-exec duplicate of the deployment-wide sparse global-RAM
// backing descriptor.  The first caller creates the memfd and fixes its
// capacity; every later caller must request the same capacity.
class GlobalRAMBacking final
{
  public:
    static int duplicate(std::uint64_t capacityBytes);
    // Host deployment loading, before any tile starts. This does not simulate
    // host upload time; subsequent tile DMA reads use the timed controller.
    static void loadImage(int descriptor, std::uint64_t capacityBytes,
                          const std::string& path, std::uint64_t offset);
};

} // namespace Mittens
} // namespace SST

#endif
