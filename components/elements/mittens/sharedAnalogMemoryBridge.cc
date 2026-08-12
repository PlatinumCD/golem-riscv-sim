#include "sharedAnalogMemoryBridge.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace SST {
namespace Mittens {

SharedAnalogMemoryBridge::~SharedAnalogMemoryBridge()
{
    close();
}

void SharedAnalogMemoryBridge::create(
    std::uint32_t tileId,
    std::uint32_t arrayCount,
    std::uint32_t arrayRows,
    std::uint32_t arrayColumns)
{
    if (open()) {
        throw std::logic_error(
            "shared analog bridge is already open");
    }
    if (arrayCount == 0 || arrayRows == 0 || arrayColumns == 0) {
        throw std::invalid_argument(
            "shared analog bridge geometry must be nonzero");
    }
    if (arrayRows >
        std::numeric_limits<std::size_t>::max() / arrayColumns) {
        throw std::overflow_error(
            "shared analog bridge array geometry is too large");
    }
    const std::size_t wordsPerSlot =
        mittens_analog_words_per_slot(arrayRows, arrayColumns);
    if (wordsPerSlot >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "shared analog bridge slot contains too many words");
    }

    mappingSize_ = mittens_analog_bridge_size(
        arrayCount, arrayRows, arrayColumns);
    if (mappingSize_ >
        static_cast<std::size_t>(
            std::numeric_limits<off_t>::max())) {
        mappingSize_ = 0;
        throw std::overflow_error(
            "shared analog bridge mapping is too large");
    }

#if defined(SYS_memfd_create)
    const std::string name =
        "mittens-analog-tile-" + std::to_string(tileId);
    fileDescriptor_ = static_cast<int>(
        syscall(SYS_memfd_create, name.c_str(), MFD_CLOEXEC));
#else
    mappingSize_ = 0;
    throw std::runtime_error(
        "memfd_create is not supported on this host");
#endif

    if (fileDescriptor_ < 0) {
        const int error = errno;
        mappingSize_ = 0;
        throw std::system_error(
            error,
            std::generic_category(),
            "memfd_create failed for Mittens analog bridge");
    }

    if (ftruncate(
            fileDescriptor_,
            static_cast<off_t>(mappingSize_)) < 0) {
        const int error = errno;
        close();
        throw std::system_error(
            error,
            std::generic_category(),
            "ftruncate failed for Mittens analog bridge");
    }

    void* const address = mmap(
        nullptr,
        mappingSize_,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fileDescriptor_,
        0);
    if (address == MAP_FAILED) {
        const int error = errno;
        mapping_ = nullptr;
        close();
        throw std::system_error(
            error,
            std::generic_category(),
            "mmap failed for Mittens analog bridge");
    }

    mapping_ = static_cast<MittensAnalogBridgeHeader*>(address);
    // ftruncate() guarantees that a newly extended memfd reads as zero.  Do
    // not fault every matrix-sized payload page into memory merely to write
    // those zeroes again.  Only control structures need eager, explicit
    // initialization; producers overwrite the declared payload words before
    // publishing a slot.
    std::memset(mapping_, 0, sizeof(*mapping_));
    mapping_->magic = MITTENS_ANALOG_BRIDGE_MAGIC;
    mapping_->version = MITTENS_ANALOG_BRIDGE_VERSION;
    mapping_->structure_size = mappingSize_;
    mapping_->array_count = arrayCount;
    mapping_->array_rows = arrayRows;
    mapping_->array_columns = arrayColumns;
    mapping_->queue_capacity = MITTENS_ANALOG_QUEUE_CAPACITY;
    mapping_->words_per_slot =
        static_cast<std::uint32_t>(wordsPerSlot);
    mapping_->link_width_bits = MITTENS_ANALOG_LINK_WIDTH_BITS;
    mapping_->channel_stride =
        mittens_analog_channel_stride(arrayRows, arrayColumns);
    mapping_->slot_stride =
        mittens_analog_slot_stride(arrayRows, arrayColumns);
    mapping_->tile_id = tileId;

    for (std::uint32_t arrayId = 0;
         arrayId < arrayCount;
         ++arrayId) {
        MittensAnalogBridgeChannel* const channel =
            mittens_analog_channel(mapping_, arrayId);
        std::memset(channel, 0, sizeof(*channel));

        for (std::uint32_t sequence = 0;
             sequence < MITTENS_ANALOG_QUEUE_CAPACITY;
             ++sequence) {
            MittensAnalogBridgeSlot* const slot =
                mittens_analog_slot(mapping_, arrayId, sequence);
            std::memset(slot, 0, sizeof(*slot));
            slot->state = MITTENS_ANALOG_SLOT_FREE;
            slot->status = MITTENS_ANALOG_STATUS_SUCCESS;
        }
    }
}

void SharedAnalogMemoryBridge::close() noexcept
{
    if (mapping_ != nullptr) {
        (void)munmap(mapping_, mappingSize_);
        mapping_ = nullptr;
    }

    if (fileDescriptor_ >= 0) {
        (void)::close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
    mappingSize_ = 0;
}

std::uint32_t SharedAnalogMemoryBridge::arrayCount() const noexcept
{
    return open() ? mapping_->array_count : 0;
}

std::uint32_t
SharedAnalogMemoryBridge::protocolError() const noexcept
{
    if (!open()) {
        return MITTENS_ANALOG_BRIDGE_ERROR_NONE;
    }
    return mittens_analog_load_acquire(
        &mapping_->protocol_error);
}

std::optional<AnalogBridgeSubmission>
SharedAnalogMemoryBridge::nextSubmission(std::uint32_t arrayId)
{
    if (!open()) {
        throw std::logic_error(
            "cannot read a closed shared analog bridge");
    }
    if (arrayId >= mapping_->array_count) {
        throw std::out_of_range(
            "shared analog bridge array ID is invalid");
    }

    MittensAnalogBridgeChannel* const channel =
        mittens_analog_channel(mapping_, arrayId);
    const std::uint32_t sequence =
        mittens_analog_load_relaxed(&channel->accept_index);
    const std::uint32_t writeIndex =
        mittens_analog_load_acquire(&channel->write_index);
    if (sequence == writeIndex) {
        return std::nullopt;
    }

    MittensAnalogBridgeSlot* const candidate =
        mittens_analog_slot(mapping_, arrayId, sequence);
    const std::uint32_t state =
        mittens_analog_load_acquire(&candidate->state);
    if (state != MITTENS_ANALOG_SLOT_SUBMITTED ||
        candidate->sequence != sequence) {
        setProtocolError(MITTENS_ANALOG_BRIDGE_ERROR_BAD_SLOT);
        throw std::runtime_error(
            "shared analog bridge encountered an invalid submitted slot");
    }
    if (candidate->input_word_count > mapping_->words_per_slot) {
        setProtocolError(
            MITTENS_ANALOG_BRIDGE_ERROR_BAD_PAYLOAD);
        throw std::runtime_error(
            "shared analog bridge input exceeds slot capacity");
    }

    const std::uint32_t* const words =
        mittens_analog_slot_words_const(candidate);
    return AnalogBridgeSubmission{
        AnalogBridgeToken{arrayId, sequence},
        candidate->command,
        std::vector<std::uint32_t>(
            words,
            words + candidate->input_word_count),
    };
}

std::uint32_t SharedAnalogMemoryBridge::slotState(
    const AnalogBridgeToken& token) const
{
    if (!open()) {
        throw std::logic_error(
            "cannot access a closed shared analog bridge");
    }
    if (token.arrayId >= mapping_->array_count) {
        throw std::out_of_range(
            "shared analog bridge array ID is invalid");
    }
    const MittensAnalogBridgeSlot* const candidate =
        mittens_analog_slot_const(
            mapping_, token.arrayId, token.sequence);
    if (candidate->sequence != token.sequence) {
        return MITTENS_ANALOG_SLOT_FREE;
    }
    return mittens_analog_load_acquire(&candidate->state);
}

void SharedAnalogMemoryBridge::markAccepted(
    const AnalogBridgeToken& token)
{
    MittensAnalogBridgeSlot& accepted = slot(token);
    if (mittens_analog_load_acquire(&accepted.state) !=
            MITTENS_ANALOG_SLOT_SUBMITTED ||
        accepted.sequence != token.sequence) {
        setProtocolError(MITTENS_ANALOG_BRIDGE_ERROR_BAD_SLOT);
        throw std::runtime_error(
            "cannot accept an invalid analog bridge slot");
    }

    MittensAnalogBridgeChannel* const channel =
        mittens_analog_channel(mapping_, token.arrayId);
    const std::uint32_t expected =
        mittens_analog_load_relaxed(&channel->accept_index);
    if (expected != token.sequence) {
        setProtocolError(MITTENS_ANALOG_BRIDGE_ERROR_BAD_SLOT);
        throw std::runtime_error(
            "analog bridge acceptance order was violated");
    }

    mittens_analog_store_release(
        &accepted.state, MITTENS_ANALOG_SLOT_ACCEPTED);
    mittens_analog_store_release(
        &channel->accept_index, token.sequence + UINT32_C(1));
}

void SharedAnalogMemoryBridge::complete(
    const AnalogBridgeToken& token,
    std::uint64_t status,
    const std::vector<std::uint32_t>& outputWords)
{
    MittensAnalogBridgeSlot& completed = slot(token);
    if (mittens_analog_load_acquire(&completed.state) !=
            MITTENS_ANALOG_SLOT_ACCEPTED ||
        completed.sequence != token.sequence) {
        setProtocolError(MITTENS_ANALOG_BRIDGE_ERROR_BAD_SLOT);
        throw std::runtime_error(
            "cannot complete an invalid analog bridge slot");
    }
    if (outputWords.size() > mapping_->words_per_slot) {
        setProtocolError(
            MITTENS_ANALOG_BRIDGE_ERROR_BAD_PAYLOAD);
        throw std::runtime_error(
            "analog bridge output exceeds slot capacity");
    }

    std::uint32_t* const words =
        mittens_analog_slot_words(&completed);
    std::copy(outputWords.begin(), outputWords.end(), words);
    completed.output_word_count =
        static_cast<std::uint32_t>(outputWords.size());
    completed.status = status;
    mittens_analog_store_release(
        &completed.state, MITTENS_ANALOG_SLOT_COMPLETED);
}

MittensAnalogBridgeSlot& SharedAnalogMemoryBridge::slot(
    const AnalogBridgeToken& token)
{
    if (!open()) {
        throw std::logic_error(
            "cannot access a closed shared analog bridge");
    }
    if (token.arrayId >= mapping_->array_count) {
        throw std::out_of_range(
            "shared analog bridge array ID is invalid");
    }
    return *mittens_analog_slot(
        mapping_, token.arrayId, token.sequence);
}

void SharedAnalogMemoryBridge::setProtocolError(
    enum MittensAnalogBridgeError error) noexcept
{
    if (!open()) {
        return;
    }
    std::uint32_t expected = MITTENS_ANALOG_BRIDGE_ERROR_NONE;
    (void)__atomic_compare_exchange_n(
        &mapping_->protocol_error,
        &expected,
        static_cast<std::uint32_t>(error),
        false,
        __ATOMIC_RELEASE,
        __ATOMIC_RELAXED);
}

} // namespace Mittens
} // namespace SST
