#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hmi::engine::detail
{
class SdkPreflightError : public std::runtime_error
{
public:
    DWORD code;
    SdkPreflightError(DWORD value, const std::string &message) : std::runtime_error(message), code(value) {}
};

inline std::string sdk_path_utf8(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

struct SdkFileHandle
{
    HANDLE value = nullptr;
    ~SdkFileHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    SdkFileHandle() = default;
    SdkFileHandle(const SdkFileHandle &) = delete;
    SdkFileHandle &operator=(const SdkFileHandle &) = delete;
};

struct SdkViewCloser
{
    void operator()(const BYTE *value) const { if (value) UnmapViewOfFile(value); }
};

struct SdkImport
{
    std::string library;
    std::string name;
    WORD ordinal = 0;
    bool by_ordinal = false;
};

// A read-only data mapping, never LoadLibrary/DONT_RESOLVE_DLL_REFERENCES.
// All file offsets, RVAs, tables and strings are checked before dereference.
class SdkPeImage
{
    std::filesystem::path path_;
    SdkFileHandle file_;
    SdkFileHandle mapping_;
    std::unique_ptr<const BYTE, SdkViewCloser> data_;
    std::size_t size_ = 0;
    DWORD headers_ = 0;
    DWORD image_size_ = 0;
    IMAGE_DATA_DIRECTORY imports_{};
    IMAGE_DATA_DIRECTORY delayed_{};
    IMAGE_DATA_DIRECTORY exports_{};
    std::vector<IMAGE_SECTION_HEADER> sections_;

    [[noreturn]] void invalid() const { throw SdkPreflightError(ERROR_BAD_EXE_FORMAT, "invalid or unsupported SDK PE image: " + sdk_path_utf8(path_)); }

    template<class T> T read(std::size_t offset) const
    {
        if (offset > size_ || sizeof(T) > size_ - offset) invalid();
        T value{};
        std::memcpy(&value, data_.get() + offset, sizeof(T));
        return value;
    }

    std::size_t offset(std::uint64_t rva, std::size_t count) const
    {
        if (rva < headers_ && rva <= size_ && count <= size_ - static_cast<std::size_t>(rva) &&
            count <= headers_ - rva) return static_cast<std::size_t>(rva);
        for (const auto &section : sections_)
        {
            if (rva < section.VirtualAddress) continue;
            const auto delta = rva - section.VirtualAddress;
            if (delta > section.SizeOfRawData || count > section.SizeOfRawData - delta) continue;
            const auto raw = static_cast<std::uint64_t>(section.PointerToRawData) + delta;
            if (raw > size_ || count > size_ - static_cast<std::size_t>(raw)) invalid();
            return static_cast<std::size_t>(raw);
        }
        invalid();
    }

    std::string string(std::uint64_t rva) const
    {
        const auto start = offset(rva, 1);
        const auto *text = reinterpret_cast<const char *>(data_.get() + start);
        const auto remaining = std::min<std::size_t>(size_ - start, 65536);
        const auto *end = static_cast<const char *>(std::memchr(text, 0, remaining));
        if (!end) invalid();
        return {text, end};
    }

    void thunks(std::vector<SdkImport> &out, const std::string &library, DWORD table) const
    {
        if (!table) invalid();
        for (std::size_t index = 0; index < size_ / sizeof(ULONGLONG); ++index)
        {
            const auto thunk = read<ULONGLONG>(offset(static_cast<std::uint64_t>(table) + index * sizeof(ULONGLONG), sizeof(ULONGLONG)));
            if (!thunk) return;
            SdkImport imported;
            imported.library = library;
            imported.by_ordinal = (thunk & IMAGE_ORDINAL_FLAG64) != 0;
            if (imported.by_ordinal) imported.ordinal = static_cast<WORD>(thunk & 0xffff);
            else imported.name = string(thunk + sizeof(WORD));
            out.push_back(std::move(imported));
        }
        invalid();
    }

public:
    explicit SdkPeImage(const std::filesystem::path &path) : path_(path)
    {
        file_.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_.value == INVALID_HANDLE_VALUE)
            throw SdkPreflightError(GetLastError(), "cannot read SDK file: " + sdk_path_utf8(path));
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.value, &size) || size.QuadPart <= 0 ||
            static_cast<unsigned long long>(size.QuadPart) > SIZE_MAX) invalid();
        size_ = static_cast<std::size_t>(size.QuadPart);
        mapping_.value = CreateFileMappingW(file_.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_.value) throw SdkPreflightError(GetLastError(), "cannot map SDK file");
        data_.reset(static_cast<const BYTE *>(MapViewOfFile(mapping_.value, FILE_MAP_READ, 0, 0, 0)));
        if (!data_) throw SdkPreflightError(GetLastError(), "cannot view SDK file");
        const auto dos = read<IMAGE_DOS_HEADER>(0);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) invalid();
        const auto nt = static_cast<std::size_t>(dos.e_lfanew);
        if (read<DWORD>(nt) != IMAGE_NT_SIGNATURE) invalid();
        const auto file = read<IMAGE_FILE_HEADER>(nt + sizeof(DWORD));
        if (file.Machine != IMAGE_FILE_MACHINE_AMD64 || file.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) invalid();
        const auto optional = read<IMAGE_OPTIONAL_HEADER64>(nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
        if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) invalid();
        headers_ = optional.SizeOfHeaders;
        image_size_ = optional.SizeOfImage;
        imports_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        delayed_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        exports_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const auto section_start = nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + file.SizeOfOptionalHeader;
        if (file.NumberOfSections > size_ / sizeof(IMAGE_SECTION_HEADER)) invalid();
        for (std::size_t index = 0; index < file.NumberOfSections; ++index)
            sections_.push_back(read<IMAGE_SECTION_HEADER>(section_start + index * sizeof(IMAGE_SECTION_HEADER)));
    }

    std::vector<SdkImport> imports() const
    {
        std::vector<SdkImport> out;
        if (imports_.VirtualAddress)
        {
            bool ended = false;
            for (std::size_t index = 0; index < imports_.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++index)
            {
                const auto entry = read<IMAGE_IMPORT_DESCRIPTOR>(offset(static_cast<std::uint64_t>(imports_.VirtualAddress) + index * sizeof(IMAGE_IMPORT_DESCRIPTOR), sizeof(IMAGE_IMPORT_DESCRIPTOR)));
                if (!entry.Name && !entry.FirstThunk && !entry.OriginalFirstThunk) { ended = true; break; }
                thunks(out, string(entry.Name), entry.OriginalFirstThunk ? entry.OriginalFirstThunk : entry.FirstThunk);
            }
            if (!ended) invalid();
        }
        if (delayed_.VirtualAddress)
        {
            struct Delay { DWORD attributes, name, module, address_table, name_table, bound, unload, timestamp; };
            bool ended = false;
            for (std::size_t index = 0; index < delayed_.Size / sizeof(Delay); ++index)
            {
                const auto entry = read<Delay>(offset(static_cast<std::uint64_t>(delayed_.VirtualAddress) + index * sizeof(Delay), sizeof(Delay)));
                if (!entry.name) { ended = true; break; }
                if (entry.attributes != 1) invalid();
                thunks(out, string(entry.name), entry.name_table);
            }
            if (!ended) invalid();
        }
        return out;
    }

    bool exports(const SdkImport &imported) const
    {
        if (!exports_.VirtualAddress) return false;
        const auto table = read<IMAGE_EXPORT_DIRECTORY>(offset(exports_.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
        if (table.NumberOfFunctions > size_ / sizeof(DWORD) || table.NumberOfNames > size_ / sizeof(DWORD)) invalid();
        DWORD index = table.NumberOfFunctions;
        if (imported.by_ordinal)
        {
            if (imported.ordinal < table.Base) return false;
            index = imported.ordinal - table.Base;
        }
        else
            for (DWORD name = 0; name < table.NumberOfNames; ++name)
            {
                const auto rva = read<DWORD>(offset(static_cast<std::uint64_t>(table.AddressOfNames) + name * sizeof(DWORD), sizeof(DWORD)));
                if (string(rva) == imported.name)
                {
                    index = read<WORD>(offset(static_cast<std::uint64_t>(table.AddressOfNameOrdinals) + name * sizeof(WORD), sizeof(WORD)));
                    break;
                }
            }
        if (index >= table.NumberOfFunctions) return false;
        const auto address = read<DWORD>(offset(static_cast<std::uint64_t>(table.AddressOfFunctions) + index * sizeof(DWORD), sizeof(DWORD)));
        if (!address) return false;
        if (address >= exports_.VirtualAddress && static_cast<std::uint64_t>(address) < static_cast<std::uint64_t>(exports_.VirtualAddress) + exports_.Size)
            throw SdkPreflightError(ERROR_NOT_SUPPORTED, "forwarded SDK export is not supported by static preflight: " + sdk_path_utf8(path_.filename()));
        // Export addresses may refer to variables in a zero-filled virtual
        // section tail. Only metadata needs raw file backing; target addresses
        // must instead lie within the mapped image/section bounds.
        if (address >= image_size_) invalid();
        bool mapped = address < headers_;
        for (const auto &section : sections_)
            if (address >= section.VirtualAddress && static_cast<std::uint64_t>(address) <
                static_cast<std::uint64_t>(section.VirtualAddress) + std::max(section.Misc.VirtualSize, section.SizeOfRawData)) mapped = true;
        if (!mapped) invalid();
        return true;
    }
};

inline std::wstring sdk_fold_path(const std::filesystem::path &path)
{
    auto text = path.lexically_normal().wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return text;
}

inline bool sdk_preflight(const std::filesystem::path &library, std::string *error)
{
    try
    {
        const auto directory = library.parent_path();
        std::vector<std::filesystem::path> pending{library};
        std::error_code code;
        for (std::filesystem::directory_iterator it(directory, code), end; it != end; it.increment(code))
        {
            if (code) throw SdkPreflightError(ERROR_PATH_NOT_FOUND, "cannot enumerate SDK directory");
            const auto name = sdk_fold_path(it->path().filename());
            if (it->is_regular_file(code) && (name.starts_with(L"ggml") || name.starts_with(L"cudnn")) && name.ends_with(L".dll"))
                pending.push_back(it->path());
        }
        std::unordered_set<std::wstring> visited;
        std::unordered_map<std::wstring, std::unique_ptr<SdkPeImage>> images;
        const auto image = [&images](const std::filesystem::path &path) -> const SdkPeImage & {
            const auto key = sdk_fold_path(path);
            auto found = images.find(key);
            if (found == images.end()) found = images.emplace(key, std::make_unique<SdkPeImage>(path)).first;
            return *found->second;
        };
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const auto current = pending[index];
            if (!visited.insert(sdk_fold_path(current)).second) continue;
            for (const auto &imported : image(current).imports())
            {
                const std::filesystem::path name(imported.library);
                if (name.empty() || name.filename() != name || name.has_parent_path())
                    throw SdkPreflightError(ERROR_BAD_EXE_FORMAT, "SDK import is not a DLL basename");
                const auto target = directory / name;
                const bool exists = std::filesystem::exists(target, code);
                const bool local = exists && std::filesystem::is_regular_file(target, code);
                if (code) throw SdkPreflightError(ERROR_ACCESS_DENIED, "cannot inspect SDK dependency: " + sdk_path_utf8(target));
                if (!local)
                {
                    const auto folded = sdk_fold_path(name);
                    if (folded.starts_with(L"ggml") || folded.starts_with(L"cudnn"))
                        throw SdkPreflightError(ERROR_MOD_NOT_FOUND, "SDK dependency is missing: " + imported.library);
                    // Installed CUDA/system runtimes retain normal loader/PATH lookup.
                    continue;
                }
                if (!image(target).exports(imported))
                    throw SdkPreflightError(ERROR_PROC_NOT_FOUND, "SDK import mismatch: " + sdk_path_utf8(current.filename()) +
                        " requires " + imported.library + "!" + (imported.by_ordinal ? "#" + std::to_string(imported.ordinal) : imported.name));
                const HMODULE loaded = GetModuleHandleW(name.c_str());
                if (loaded)
                {
                    wchar_t path[32768]{};
                    const DWORD count = GetModuleFileNameW(loaded, path, 32768);
                    if (!count || sdk_fold_path(std::filesystem::path(std::wstring(path, count))) != sdk_fold_path(target))
                        throw SdkPreflightError(ERROR_PROC_NOT_FOUND, "SDK dependency already loaded from a different directory: " + imported.library);
                }
                pending.push_back(target);
            }
        }
        return true;
    }
    catch (const SdkPreflightError &failure)
    {
        if (error) *error = "SDK preflight failed (win32 " + std::to_string(failure.code) + "): " + failure.what();
        SetLastError(failure.code);
    }
    catch (const std::exception &failure)
    {
        if (error) *error = std::string("SDK preflight failed: ") + failure.what();
        SetLastError(ERROR_BAD_EXE_FORMAT);
    }
    return false;
}
}
