#pragma once

#include <sst/core/params.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace TileComponents {

// Connections select physical banks in one address space. They never remap
// addresses or create an additional bank-port/bandwidth pool for a client.
class BankConnections {
public:
    bool hasRouter() const { return !routerRequestor_.empty(); }
    void configure(const SST::Params& params) {
        banks_ = params.find<unsigned>("spm_banks", 8);
        width_ = params.find<unsigned>("spm_bank_width", 4);
        if (!banks_ || banks_ > static_cast<unsigned>(std::numeric_limits<int>::max()) || !width_)
            throw std::invalid_argument("invalid SPM bank geometry");
        cpuRequestor_ = params.find<std::string>("cpu_requestor", "");
        routerRequestor_ = params.find<std::string>("router_requestor", "");
        if (!cpuRequestor_.empty() && cpuRequestor_ == routerRequestor_)
            throw std::invalid_argument("CPU and router SPM requestors must be distinct");
        cpu_ = parse(params, "cpu_spm_banks", true);
        router_ = parse(params, "router_spm_banks", false);
    }

    // -1: connected; -2: unknown requestor in a router-connected tile;
    // -3: invalid byte range; otherwise the first disconnected physical bank.
    int deniedBank(const std::string& requestor, std::uint64_t address,
                   std::uint64_t bytes) const {
        if (!bytes || bytes - 1 > std::numeric_limits<std::uint64_t>::max() - address)
            return -3;
        const std::vector<bool>* connected = nullptr;
        if (!cpuRequestor_.empty() && requestor == cpuRequestor_) connected = &cpu_;
        else if (!routerRequestor_.empty() && requestor == routerRequestor_) connected = &router_;
        else if (!routerRequestor_.empty()) return -2;
        // Preserve standalone test clients with no router role binding.
        if (!connected) return -1;
        const auto first = address / width_;
        const auto stripes = std::min<std::uint64_t>(banks_, (address + bytes - 1) / width_ - first + 1);
        for (std::uint64_t stripe = 0; stripe < stripes; ++stripe) {
            const unsigned bank = (first + stripe) % banks_;
            if (!(*connected)[bank]) return static_cast<int>(bank);
        }
        return -1;
    }

private:
    std::vector<bool> parse(const SST::Params& params, const char* name, bool fallback) const {
        std::vector<bool> connected(banks_, fallback && !params.contains(name));
        if (!params.contains(name)) return connected;
        std::vector<int> identifiers;
        params.find_array<int>(name, identifiers);
        for (const int bank : identifiers) {
            if (bank < 0 || static_cast<unsigned>(bank) >= banks_ || connected[bank])
                throw std::invalid_argument(std::string(name) + " requires unique physical bank IDs in range");
            connected[bank] = true;
        }
        return connected;
    }
    unsigned banks_ = 8, width_ = 4;
    std::string cpuRequestor_, routerRequestor_;
    std::vector<bool> cpu_, router_;
};
}
