#pragma once
#include <cstdint>
#include <string>
#include "tileParameters.h"
namespace SST
{
class Params;
class Output;
} // namespace SST
namespace SST::Mittens
{
// Immutable after construction. Controllers consume resolved settings.
struct TileConfiguration
{
#define MITTENS_FIELD(type, field, name, value, category, help, documented) type field = value;
    MITTENS_TILE_PARAMETERS(MITTENS_FIELD)
#undef MITTENS_FIELD
    static TileConfiguration read(SST::Params& params);
    void validate(SST::Output& output) const;
    std::string writeResolved(const std::string& directory) const;
};
} // namespace SST::Mittens
