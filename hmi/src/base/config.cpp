#include "base/config.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace hmi::base
{
namespace
{
constexpr std::size_t max_file_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_depth = 64;
constexpr std::size_t max_nodes = 65536;

std::string display_path(const std::filesystem::path &path)
{
    try
    {
        const auto text = path.u8string();
        return {reinterpret_cast<const char *>(text.data()), text.size()};
    }
    catch (const std::filesystem::filesystem_error &) { return "<unrepresentable path>"; }
}

void validate_file_path(const std::filesystem::path &path)
{
    if (path.empty() || path.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
        throw std::invalid_argument("configuration file path must be non-empty and contain no NUL");
}

std::vector<std::string> path_parts(std::string_view path)
{
    if (path.empty() || path.size() > 1024 || path.find('\0') != std::string_view::npos)
        throw std::invalid_argument("configuration path must contain 1..1024 bytes without NUL");
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const auto dot = path.find('.', start);
        const auto end = dot == std::string_view::npos ? path.size() : dot;
        if (end == start || result.size() >= max_depth)
            throw std::invalid_argument("configuration path must contain 1..64 non-empty mapping keys");
        result.emplace_back(path.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return result;
}

struct Budget
{
    std::size_t nodes = 0;
    std::size_t bytes = 0;
    std::vector<YAML::Node> ancestors;

    void consume(std::size_t size)
    {
        if (++nodes > max_nodes || size > max_file_bytes - bytes)
            throw std::invalid_argument("configuration exceeds expanded node or scalar size limit");
        bytes += size;
    }
};

// Normalize aliases into independent values rather than retaining shared YAML
// identities. This keeps later set() operations local to their dotted path.
YAML::Node normalized(const YAML::Node &node, Budget &budget, std::size_t depth)
{
    if (depth > max_depth)
        throw std::invalid_argument("configuration exceeds maximum nesting depth of 64");
    if (!node.IsDefined()) throw std::invalid_argument("undefined YAML nodes cannot be stored");
    budget.consume(node.IsScalar() ? node.Scalar().size() : 0);
    if (node.IsNull() || node.IsScalar()) return YAML::Clone(node);
    for (const auto &ancestor : budget.ancestors)
        if (node.is(ancestor)) throw std::invalid_argument("recursive YAML aliases are not supported");
    budget.ancestors.push_back(node);
    YAML::Node copy(node.IsMap() ? YAML::NodeType::Map : YAML::NodeType::Sequence);
    if (node.IsMap())
    {
        std::unordered_set<std::string> keys;
        for (auto entry : node)
        {
            if (!entry.first.IsScalar() || entry.first.Scalar().empty())
                throw std::invalid_argument("configuration mapping keys must be non-empty scalars");
            const auto key = entry.first.Scalar();
            budget.consume(key.size());
            if (!keys.insert(key).second)
                throw std::invalid_argument("duplicate configuration mapping key: " + key);
            copy[key] = normalized(entry.second, budget, depth + 1);
        }
    }
    else
        for (auto entry : node) copy.push_back(normalized(entry, budget, depth + 1));
    budget.ancestors.pop_back();
    return copy;
}

YAML::Node normalized(const YAML::Node &node)
{
    Budget budget;
    return normalized(node, budget, 0);
}

std::string read_text(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open configuration file");
    std::string text;
    std::array<char, 8192> buffer{};
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = static_cast<std::size_t>(file.gcount());
        if (bytes > max_file_bytes - text.size())
            throw std::runtime_error("configuration file exceeds 4 MiB");
        text.append(buffer.data(), bytes);
    }
    if (file.bad()) throw std::runtime_error("cannot read complete configuration file");
    return text;
}

class TemporaryFile
{
public:
    explicit TemporaryFile(const std::filesystem::path &target)
    {
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto nonce = (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
#ifdef _WIN32
            path_ = target.parent_path() / (target.filename().native() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(nonce));
            handle_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot create configuration temporary file");
#else
            path_ = target.parent_path() / (target.filename().native() + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(nonce));
            descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (descriptor_ >= 0) return;
            if (errno != EEXIST) throw std::system_error(errno, std::generic_category(), "cannot create configuration temporary file");
#endif
        }
        throw std::runtime_error("cannot reserve unique configuration temporary file");
    }

    ~TemporaryFile()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
        if (!path_.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;

    void commit(const std::filesystem::path &target, std::string_view text)
    {
        std::size_t offset = 0;
        while (offset < text.size())
        {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(handle_, text.data() + offset, static_cast<DWORD>(text.size() - offset), &written, nullptr) || !written)
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot write configuration temporary file");
            offset += written;
#else
            const auto written = ::write(descriptor_, text.data() + offset, text.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(errno, std::generic_category(), "cannot write configuration temporary file");
            offset += static_cast<std::size_t>(written);
#endif
        }
#ifdef _WIN32
        if (!FlushFileBuffers(handle_))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot flush configuration temporary file");
        if (!CloseHandle(handle_))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot close configuration temporary file");
        handle_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(path_.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot replace configuration file");
#else
        if (::fsync(descriptor_) != 0)
            throw std::system_error(errno, std::generic_category(), "cannot flush configuration temporary file");
        const int closed = ::close(descriptor_);
        descriptor_ = -1;
        if (closed != 0) throw std::system_error(errno, std::generic_category(), "cannot close configuration temporary file");
        if (::rename(path_.c_str(), target.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "cannot replace configuration file");
#endif
        path_.clear();
    }

private:
    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};
}

bool Config::load(const std::filesystem::path &path, std::string *error)
{
    try
    {
        std::lock_guard file_lock(file_mutex_);
        validate_file_path(path);
        auto parsed = normalized(YAML::Load(read_text(path)));
        if (!parsed.IsMap()) throw std::invalid_argument("configuration root must be a YAML mapping");
        std::unique_lock lock(mutex_);
        root_.reset(parsed);
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &failure)
    {
        if (error) *error = "cannot load configuration '" + display_path(path) + "': " + failure.what();
        return false;
    }
}

bool Config::save(const std::filesystem::path &path, std::string *error) const
{
    try
    {
        std::lock_guard file_lock(file_mutex_);
        validate_file_path(path);
        YAML::Emitter emitter;
        emitter.SetIndent(2);
        emitter << snapshot();
        if (!emitter.good()) throw std::runtime_error("cannot encode configuration YAML: " + emitter.GetLastError());
        std::string text = emitter.c_str();
        text += '\n';
        if (text.size() > max_file_bytes) throw std::runtime_error("serialized configuration exceeds 4 MiB");
        const auto target = std::filesystem::absolute(path).lexically_normal();
        std::filesystem::create_directories(target.parent_path());
        TemporaryFile temporary(target);
        temporary.commit(target, text);
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &failure)
    {
        if (error) *error = "cannot save configuration '" + display_path(path) + "': " + failure.what();
        return false;
    }
}

YAML::Node Config::value(std::string_view path, std::string *error) const
{
    try
    {
        const auto parts = path_parts(path);
        std::shared_lock lock(mutex_);
        YAML::Node current = root_;
        for (const auto &part : parts)
        {
            if (!current.IsMap())
            {
                if (error) error->clear();
                return YAML::Node(YAML::NodeType::Undefined);
            }
            const auto next = static_cast<const YAML::Node &>(current)[part];
            if (!next.IsDefined())
            {
                if (error) error->clear();
                return YAML::Node(YAML::NodeType::Undefined);
            }
            current.reset(next);
        }
        if (error) error->clear();
        return YAML::Clone(current);
    }
    catch (const std::exception &failure)
    {
        if (error) *error = "cannot read configuration value '" + std::string(path) + "': " + failure.what();
        return YAML::Node(YAML::NodeType::Undefined);
    }
}

YAML::Node Config::snapshot() const
{
    std::shared_lock lock(mutex_);
    return YAML::Clone(root_);
}

bool Config::set_node(std::string_view path, const YAML::Node &value, std::string *error)
{
    try
    {
        const auto parts = path_parts(path);
        const auto detached = normalized(value);
        std::unique_lock lock(mutex_);
        auto candidate = YAML::Clone(root_);
        YAML::Node current = candidate;
        for (std::size_t index = 0; index + 1 < parts.size(); ++index)
        {
            auto child = current[parts[index]];
            if (!child.IsDefined() || child.IsNull()) current[parts[index]] = YAML::Node(YAML::NodeType::Map);
            else if (!child.IsMap()) throw std::invalid_argument("configuration path crosses a non-mapping value");
            current.reset(current[parts[index]]);
        }
        current[parts.back()] = detached;
        // The whole document must still fit after appending or replacing a node.
        auto checked = normalized(candidate);
        root_.reset(checked);
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &failure)
    {
        if (error) *error = "cannot set configuration value '" + std::string(path) + "': " + failure.what();
        return false;
    }
}
}
