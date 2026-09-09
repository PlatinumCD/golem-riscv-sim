#ifndef SST_MITTENS_GLOBAL_RAM_READINESS_H
#define SST_MITTENS_GLOBAL_RAM_READINESS_H

#include <cstddef>
#include <cstdint>
#include <map>

namespace SST {
namespace Mittens {

// An execution's committed global-RAM bytes.  Adjacent publications coalesce,
// but any overlapping publication is rejected because V1 is single-assignment.
class GlobalRAMCommittedRanges final
{
  public:
    enum class PublishResult {
        Published,
        Overlap,
    };

    bool covers(std::uint64_t begin, std::uint64_t end) const noexcept
    {
        if (begin >= end) {
            return false;
        }
        auto candidate = ranges_.upper_bound(begin);
        if (candidate == ranges_.begin()) {
            return false;
        }
        --candidate;
        return candidate->first <= begin && candidate->second >= end;
    }

    bool coveringRange(
        std::uint64_t point,
        std::uint64_t* begin,
        std::uint64_t* end) const noexcept
    {
        auto candidate = ranges_.upper_bound(point);
        if (candidate == ranges_.begin()) {
            return false;
        }
        --candidate;
        if (candidate->second <= point) {
            return false;
        }
        if (begin != nullptr) {
            *begin = candidate->first;
        }
        if (end != nullptr) {
            *end = candidate->second;
        }
        return true;
    }

    PublishResult publish(std::uint64_t begin, std::uint64_t end)
    {
        if (begin >= end) {
            return PublishResult::Overlap;
        }

        auto right = ranges_.lower_bound(begin);
        if (right != ranges_.end() && right->first < end) {
            return PublishResult::Overlap;
        }

        auto left = right;
        if (left != ranges_.begin()) {
            --left;
            if (left->second > begin) {
                return PublishResult::Overlap;
            }
        } else {
            left = ranges_.end();
        }

        const bool joinsLeft =
            left != ranges_.end() && left->second == begin;
        const bool joinsRight =
            right != ranges_.end() && right->first == end;
        if (joinsLeft && joinsRight) {
            left->second = right->second;
            ranges_.erase(right);
        } else if (joinsLeft) {
            left->second = end;
        } else if (joinsRight) {
            const std::uint64_t rightEnd = right->second;
            ranges_.erase(right);
            ranges_.emplace(begin, rightEnd);
        } else {
            ranges_.emplace(begin, end);
        }
        return PublishResult::Published;
    }

    std::size_t size() const noexcept { return ranges_.size(); }

  private:
    std::map<std::uint64_t, std::uint64_t> ranges_;
};

} // namespace Mittens
} // namespace SST

#endif
