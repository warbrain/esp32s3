#pragma once

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>

namespace hmi::base
{
// Owns a YAML mapping. Public nodes are detached copies, so readers cannot
// mutate internal state after a lock has been released. A failed load or set
// leaves the last valid mapping intact. Dotted paths address mapping keys.
class Config
{
public:
    Config() = default;
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    bool load(const std::filesystem::path &path, std::string *error = nullptr);
    bool save(const std::filesystem::path &path, std::string *error = nullptr) const;

    // Missing values return Undefined and clear error. Invalid paths or other
    // failures return Undefined and describe the problem when error is given.
    YAML::Node value(std::string_view path, std::string *error = nullptr) const;
    YAML::Node snapshot() const;
    bool set_node(std::string_view path, const YAML::Node &value, std::string *error = nullptr);

    template<class T>
    bool try_get(std::string_view path, T &out, std::string *error = nullptr) const
    {
        std::string detail;
        const auto selected = value(path, &detail);
        if (!selected.IsDefined())
        {
            if (error) *error = detail.empty() ? "configuration value not found: " + std::string(path) : detail;
            return false;
        }
        try
        {
            T converted = selected.as<T>();
            out = std::move(converted);
            if (error) error->clear();
            return true;
        }
        catch (const std::exception &failure)
        {
            if (error) *error = "invalid configuration value '" + std::string(path) + "': " + failure.what();
            return false;
        }
    }

    template<class T>
    T get(std::string_view path, const T &fallback) const
    {
        T result = fallback;
        return try_get(path, result) ? result : fallback;
    }

    template<class T>
    bool set(std::string_view path, const T &value, std::string *error = nullptr)
    {
        try { return set_node(path, YAML::Node(value), error); }
        catch (const std::exception &failure)
        {
            if (error) *error = "cannot set configuration value '" + std::string(path) + "': " + failure.what();
            return false;
        }
    }

private:
    mutable std::mutex file_mutex_;
    mutable std::shared_mutex mutex_;
    YAML::Node root_{YAML::NodeType::Map};
};
}
