#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "../../components/mordred/spmRequest.h"
#include "layout.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace TileComponents {
/* A test-only network client. It cannot access a backing file or StandardMem;
 * every read, write and flag observation goes through the endpoint/network. */
class SharedBankInitiator final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(SharedBankInitiator, "tilecomponents", "SharedBankInitiator",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Shared-bank network regression client", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS(
        {"tile_id", "Origin tile", "0"}, {"banks", "Physical bank count", "4"},
        {"bank_width", "Bytes per bank word", "4"},
        {"router_bank0", "First attached bank", "2"}, {"router_bank1", "Second attached bank", "3"})
    SST_ELI_DOCUMENT_PORTS({"requests", "Explicit remote memory requests", {"TileComponents.MordredSpmRequest"}})
    SharedBankInitiator(SST::ComponentId_t id, SST::Params& p) : Component(id),
        tile_(p.find<unsigned>("tile_id", 0)), target_((tile_ + 1) % 4),
        banks_(p.find<unsigned>("banks", 4)), width_(p.find<unsigned>("bank_width", 4)),
        first_(p.find<unsigned>("router_bank0", 2)), second_(p.find<unsigned>("router_bank1", 3)) {
        clock_ = registerClock("1GHz", new SST::Clock::Handler<SharedBankInitiator, &SharedBankInitiator::tick>(this));
        link_ = configureLink("requests", clock_,
            new SST::Event::Handler<SharedBankInitiator, &SharedBankInitiator::response>(this));
        check(link_ && banks_ == 4 && width_ >= 4 && width_ % 4 == 0 && first_ != second_, "bad fixture configuration");
        const char* directory = std::getenv("TILE_COMPONENT_OUTPUT");
        check(directory != nullptr, "missing trace directory");
        trace_.open(std::string(directory) + "/initiator" + std::to_string(tile_) + ".csv");
        check(bool(trace_), "cannot open trace");
        trace_ << "event,cycle,id,label,source,destination,address,bytes,write,status,data_hex\n";
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void finish() override {
        check(done_ && pending_.empty() && readWords_ == TEST_WORDS && writtenWords_ == TEST_WORDS,
              "fixture terminated before all requests and guest checks completed");
        check(rejections_ == 8 && busy_ == 1, "negative requests were not rejected exactly once");
        std::cout << "SHARED_BANK_CLIENT_STATS {\"tile\":" << tile_ << ",\"destination\":" << target_
                  << ",\"passed\":true,\"requests\":" << issued_ << ",\"responses\":" << responded_
                  << ",\"source_bytes_checked\":" << readWords_ * 4 << ",\"input_bytes_written\":" << writtenWords_ * 4
                  << ",\"rejections\":" << rejections_ << ",\"busy\":" << busy_
                  << ",\"max_pending\":" << maxPending_ << ",\"end_cycle\":" << now() << "}\n";
    }
private:
    struct Pending {
        MordredSpmRequest request;
        std::string label;
        std::vector<std::uint8_t> expected;
        std::uint32_t status;
        unsigned word;
    };
    SST::Link* link_ = nullptr;
    SST::TimeConverter clock_;
    unsigned tile_, target_, banks_, width_, first_, second_;
    unsigned stage_ = 0, nextWord_ = 0, nextNegative_ = 0;
    unsigned readWords_ = 0, writtenWords_ = 0, rejections_ = 0, busy_ = 0, maxPending_ = 0;
    std::uint64_t issued_ = 0, responded_ = 0;
    bool busySent_ = false, done_ = false;
    std::map<std::uint64_t, Pending> pending_;
    std::ofstream trace_;
    std::uint64_t now() const { return getCurrentSimTime(clock_); }
    void check(bool value, const char* message) {
        if (!value) fatal(CALL_INFO, -1, "shared-bank initiator %u stage %u: %s\n", tile_, stage_, message);
    }
    static std::vector<std::uint8_t> word(std::uint32_t value) {
        return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
    }
    static std::uint32_t decode(const std::vector<std::uint8_t>& bytes) {
        return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
               (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    }
    std::vector<std::uint8_t> payload(unsigned owner, unsigned first, unsigned count) {
        std::vector<std::uint8_t> bytes;
        for (unsigned i = first; i < first + count; ++i) {
            auto next = word(test_float_bits(owner, i)); bytes.insert(bytes.end(), next.begin(), next.end());
        }
        return bytes;
    }
    unsigned address(unsigned base, unsigned index) {
        return test_word_offset(base, index, banks_, width_, first_, second_);
    }
    void trace(const char* event, const MordredSpmRequest& request, const std::string& label) {
        std::ostringstream bytes;
        for (auto byte : request.data) bytes << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
        trace_ << event << ',' << now() << ',' << request.requestId << ',' << label << ',' << tile_ << ','
               << request.destinationTile << ',' << request.address << ',' << request.bytes << ',' << request.write << ','
               << request.status << ',' << bytes.str() << '\n'; trace_.flush();
    }
    void issue(const std::string& label, unsigned location, unsigned bytes, bool write,
               std::vector<std::uint8_t> data = {}, unsigned status = 0,
               std::vector<std::uint8_t> expected = {}, unsigned firstWord = 0) {
        auto* request = new MordredSpmRequest;
        request->requestId = ++issued_; request->destinationTile = target_;
        request->address = location; request->bytes = bytes; request->write = write;
        request->data = std::move(data);
        Pending pending{*request, label, std::move(expected), status, firstWord};
        pending_.emplace(request->requestId, std::move(pending));
        maxPending_ = std::max<unsigned>(maxPending_, pending_.size());
        trace("request", *request, label); link_->send(request);
    }
    void negative(unsigned index) {
        unsigned forbidden = 0;
        while (forbidden == first_ || forbidden == second_) ++forbidden;
        unsigned boundary = first_;
        if (((boundary + 1) % banks_) == first_ || ((boundary + 1) % banks_) == second_) boundary = second_;
        const unsigned privateAddress = TEST_PROBE_OFFSET + forbidden * width_;
        const unsigned mixedAddress = TEST_PROBE_OFFSET + boundary * width_;
        if (index < 2) issue("forbidden", privateAddress, width_, index == 1,
            index == 1 ? std::vector<std::uint8_t>(width_, 0xa5) : std::vector<std::uint8_t>{}, 3);
        else if (index < 4) issue("mixed", mixedAddress, 2 * width_, index == 3,
            index == 3 ? std::vector<std::uint8_t>(2 * width_, 0xa5) : std::vector<std::uint8_t>{}, 3);
        else if (index < 6) issue("bounds", (2u << 20) - 2, 4, index == 5,
            index == 5 ? std::vector<std::uint8_t>(4, 0xa5) : std::vector<std::uint8_t>{}, 2);
        else if (index == 6) issue("malformed", address(TEST_PROBE_OFFSET, 0), 4, true, {}, 1);
        else issue("zero-length", address(TEST_PROBE_OFFSET, 0), 0, false, {}, 1);
    }
    bool tick(SST::Cycle_t) {
        check(now() < 1000000, "progress timeout");
        if (done_) return true;
        if (stage_ == 0 && pending_.empty()) issue("ready", TEST_READY_OFFSET + first_ * width_, 4, false);
        else if (stage_ == 1) {
            while (pending_.size() < 4 && nextWord_ < TEST_WORDS) {
                issue("source", address(TEST_SOURCE_OFFSET, nextWord_), width_, false, {}, 0,
                      payload(target_, nextWord_, width_ / 4), nextWord_);
                nextWord_ += width_ / 4;
            }
            if (!busySent_ && pending_.size() == 4) {
                issue("window-full", address(TEST_SOURCE_OFFSET, 0), width_, false, {}, 4);
                busySent_ = true;
            }
            if (nextWord_ == TEST_WORDS && pending_.empty()) { stage_ = 2; nextWord_ = 0; }
        } else if (stage_ == 2) {
            while (pending_.size() < 4 && nextNegative_ < 8) negative(nextNegative_++);
            if (nextNegative_ == 8 && pending_.empty()) stage_ = 3;
        } else if (stage_ == 3) {
            while (pending_.size() < 4 && nextWord_ < TEST_WORDS) {
                issue("input", address(TEST_INPUT_OFFSET, nextWord_), width_, true,
                      payload(tile_, nextWord_, width_ / 4), 0, {}, nextWord_);
                nextWord_ += width_ / 4;
            }
            if (nextWord_ == TEST_WORDS && pending_.empty()) stage_ = 4;
        } else if (stage_ == 4 && pending_.empty()) {
            issue("notify", TEST_NOTIFY_OFFSET + first_ * width_, 4, true, word(TEST_NOTIFY_VALUE | target_));
            stage_ = 5;
        } else if (stage_ == 5 && pending_.empty()) issue("done", TEST_DONE_OFFSET + first_ * width_, 4, false);
        return done_;
    }
    void response(SST::Event* event) {
        auto* response = dynamic_cast<MordredSpmRequest*>(event);
        check(response != nullptr && response->response, "wrong response event");
        auto found = pending_.find(response->requestId);
        check(found != pending_.end(), "unknown/duplicate response identity");
        const auto request = found->second;
        check(response->sourceTile == tile_ && response->destinationTile == target_ &&
              response->address == request.request.address && response->bytes == request.request.bytes &&
              response->write == request.request.write, "response identity changed");
        check(response->status == request.status, "unexpected response status");
        trace("response", *response, request.label); ++responded_;
        if (response->status) {
            check(response->data.empty(), "error response included data");
            if (response->status == 4) ++busy_; else ++rejections_;
        } else if (response->write) {
            check(response->data.empty(), "write response included data");
            if (request.label == "input") writtenWords_ += response->bytes / 4;
        } else {
            check(response->data.size() == response->bytes, "short read response");
            if (request.label == "ready") {
                const auto value = decode(response->data);
                check(value == 0 || value == (TEST_READY_VALUE | target_), "invalid ready flag");
                if (value) stage_ = 1;
            } else if (request.label == "done") {
                const auto value = decode(response->data);
                check(value == 0 || value == (TEST_DONE_VALUE | target_), "invalid done flag");
                if (value) { done_ = true; primaryComponentOKToEndSim(); }
            } else {
                check(response->data == request.expected, "remote read differs from CPU-written FP32 pattern");
                readWords_ += response->bytes / 4;
            }
        }
        pending_.erase(found); delete response;
    }
};
}
