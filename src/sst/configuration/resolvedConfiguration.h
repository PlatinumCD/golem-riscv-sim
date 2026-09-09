#pragma once
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>
#include <cstdint>
#include <unistd.h>

namespace SST::Mittens
{

// JSON emission shared by all resource configurations. This is host-only work.
class ConfigurationWriter
{
  public:
    explicit ConfigurationWriter(std::ostream& stream) : stream_(stream) {}
    static void string(std::ostream& out, const std::string& value)
    {
        constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (const unsigned char c : value)
        {
            if (c == '"' || c == '\\')
                out << '\\' << c;
            else if (c < 32)
                out << "\\u00" << hex[c >> 4] << hex[c & 15];
            else
                out << c;
        }
        out << '"';
    }
    template <class T> void add(const char* name, const char* category, const T& value)
    {
        stream_ << (first_ ? "\n    " : ",\n    ");
        first_ = false;
        string(stream_, name);
        stream_ << ": {\"category\": ";
        string(stream_, category);
        stream_ << ", \"value\": ";
        if constexpr (std::is_same_v<T, std::string>)
            string(stream_, value);
        else if constexpr (std::is_same_v<T, bool>)
            stream_ << (value ? "true" : "false");
        else if constexpr (std::is_same_v<T, std::vector<std::uint32_t>>)
        {
            stream_ << '[';
            bool first = true;
            for (const auto element : value)
            {
                if (!first)
                    stream_ << ',';
                first = false;
                stream_ << element;
            }
            stream_ << ']';
        }
        else
            stream_ << value;
        stream_ << '}';
    }

  private:
    std::ostream& stream_;
    bool first_ = true;
};

inline std::atomic<unsigned long long> configurationSequence{0};

template <class Visitor>
std::string writeResolvedConfiguration(const std::string& directory, const std::string& resource,
                                       unsigned long long id, Visitor visit)
{
    if (directory.empty())
        return {};
    std::filesystem::create_directories(directory);
    const auto path = std::filesystem::path(directory) /
                      (resource + "-" + std::to_string(id) + "-pid-" + std::to_string(getpid()) +
                       "-" + std::to_string(configurationSequence++) + ".json");
    std::ofstream out(path);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << "{\n  \"schema_version\": 1,\n  \"parameters\": {";
    ConfigurationWriter writer(out);
    visit(writer);
    out << "\n  }\n}\n";
    out.close();
    return std::filesystem::absolute(path).string();
}

template <class Visitor>
void emitResolvedConfiguration(const std::string& resource, unsigned long long id, Visitor visit)
{
    if (const char* directory = std::getenv("GOLEM_RESOLVED_CONFIG_DIR"))
        writeResolvedConfiguration(directory, resource, id, visit);
}
} // namespace SST::Mittens
