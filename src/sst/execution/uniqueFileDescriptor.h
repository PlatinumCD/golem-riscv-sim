#pragma once
#include <unistd.h>
#include <utility>
namespace SST::Mittens
{
// Owns only this process's duplicate, never the deployment backing descriptor.
class UniqueFileDescriptor final
{
  public:
    explicit UniqueFileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFileDescriptor()
    {
        reset();
    }
    UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept : fd_(other.release()) {}
    UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }
    int get() const noexcept
    {
        return fd_;
    }
    int release() noexcept
    {
        return std::exchange(fd_, -1);
    }
    void reset(int fd = -1) noexcept
    {
        if (fd_ == fd)
            return;
        if (fd_ >= 0)
            (void)::close(fd_);
        fd_ = fd;
    }

  private:
    int fd_;
};
} // namespace SST::Mittens
