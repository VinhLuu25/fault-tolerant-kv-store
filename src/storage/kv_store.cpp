#include "storage/kv_store.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

namespace ftkv::storage {
namespace {

constexpr std::array<char, 8> kFileMagic{'F', 'T', 'K', 'V', 'S', 'T', 'R', '1'};
constexpr std::uint64_t kMaximumEntries = 1'000'000;
constexpr std::uint64_t kMaximumFieldBytes = 64U * 1024U * 1024U;

[[nodiscard]] std::string error_message(const std::string_view action,
                                        const std::filesystem::path& path) {
    return std::string{action} + ": " + path.string();
}

void write_exact(std::ostream& output, const char* data, const std::size_t size) {
    output.write(data, static_cast<std::streamsize>(size));
    if (!output) {
        throw StorageError{"failed to write storage snapshot"};
    }
}

void write_uint64(std::ostream& output, const std::uint64_t value) {
    std::array<unsigned char, sizeof(value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
    write_exact(output, reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void read_exact(std::istream& input, char* data, const std::size_t size) {
    input.read(data, static_cast<std::streamsize>(size));
    if (!input) {
        throw StorageError{"storage snapshot is truncated"};
    }
}

[[nodiscard]] std::uint64_t read_uint64(std::istream& input) {
    std::array<unsigned char, sizeof(std::uint64_t)> bytes{};
    read_exact(input, reinterpret_cast<char*>(bytes.data()), bytes.size());

    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

void write_field(std::ostream& output, const std::string& field) {
    if (field.size() > kMaximumFieldBytes) {
        throw StorageError{"key or value exceeds the storage size limit"};
    }

    write_uint64(output, static_cast<std::uint64_t>(field.size()));
    write_exact(output, field.data(), field.size());
}

[[nodiscard]] std::string read_field(std::istream& input) {
    const auto size = read_uint64(input);
    if (size > kMaximumFieldBytes || size > std::numeric_limits<std::size_t>::max()) {
        throw StorageError{"invalid key or value size in storage snapshot"};
    }

    std::string field(static_cast<std::size_t>(size), '\0');
    read_exact(input, field.data(), field.size());
    return field;
}

void remove_if_present(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}  // namespace

FilePersistence::FilePersistence(std::filesystem::path file_path)
    : file_path_{std::move(file_path)} {
    if (file_path_.empty()) {
        throw std::invalid_argument{"storage file path must not be empty"};
    }
}

EntryMap FilePersistence::load() const {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(file_path_, filesystem_error);
    if (filesystem_error) {
        throw StorageError{error_message("failed to inspect storage file", file_path_)};
    }
    if (!exists) {
        return {};
    }

    std::ifstream input{file_path_, std::ios::binary};
    if (!input.is_open()) {
        throw StorageError{error_message("failed to open storage file", file_path_)};
    }

    std::array<char, kFileMagic.size()> magic{};
    read_exact(input, magic.data(), magic.size());
    if (magic != kFileMagic) {
        throw StorageError{error_message("unsupported storage file format", file_path_)};
    }

    const auto entry_count = read_uint64(input);
    if (entry_count > kMaximumEntries) {
        throw StorageError{error_message("invalid entry count in storage file", file_path_)};
    }

    EntryMap entries;
    entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        auto key = read_field(input);
        auto value = read_field(input);
        const auto [unused, inserted] = entries.emplace(std::move(key), std::move(value));
        static_cast<void>(unused);
        if (!inserted) {
            throw StorageError{error_message("duplicate key in storage file", file_path_)};
        }
    }

    if (input.peek() != std::char_traits<char>::eof()) {
        throw StorageError{error_message("unexpected data at end of storage file", file_path_)};
    }
    return entries;
}

void FilePersistence::save(const EntryMap& entries) const {
    if (entries.size() > kMaximumEntries) {
        throw StorageError{"entry count exceeds the storage size limit"};
    }

    const auto parent = file_path_.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            throw StorageError{error_message("failed to create storage directory", parent)};
        }
    }

    auto temporary_path = file_path_;
    temporary_path += ".tmp";

    try {
        std::ofstream output{temporary_path, std::ios::binary | std::ios::trunc};
        if (!output.is_open()) {
            throw StorageError{error_message("failed to open temporary storage file", temporary_path)};
        }

        write_exact(output, kFileMagic.data(), kFileMagic.size());
        write_uint64(output, static_cast<std::uint64_t>(entries.size()));
        for (const auto& [key, value] : entries) {
            write_field(output, key);
            write_field(output, value);
        }

        output.flush();
        if (!output) {
            throw StorageError{error_message("failed to flush storage file", temporary_path)};
        }
        output.close();
        if (!output) {
            throw StorageError{error_message("failed to close storage file", temporary_path)};
        }

        std::error_code filesystem_error;
        std::filesystem::rename(temporary_path, file_path_, filesystem_error);
        if (filesystem_error) {
            throw StorageError{error_message("failed to replace storage file", file_path_)};
        }
    } catch (...) {
        remove_if_present(temporary_path);
        throw;
    }
}

KvStore::KvStore(std::unique_ptr<Persistence> persistence)
    : persistence_{std::move(persistence)} {
    if (!persistence_) {
        throw std::invalid_argument{"persistence must not be null"};
    }
    entries_ = persistence_->load();
}

void KvStore::put(std::string key, std::string value) {
    const std::unique_lock lock{mutex_};
    auto updated_entries = entries_;
    updated_entries.insert_or_assign(std::move(key), std::move(value));
    // Publish the new state only after its durable write succeeds.
    persistence_->save(updated_entries);
    entries_.swap(updated_entries);
}

std::optional<std::string> KvStore::get(const std::string_view key) const {
    const std::shared_lock lock{mutex_};
    const auto entry = entries_.find(std::string{key});
    if (entry == entries_.end()) {
        return std::nullopt;
    }
    return entry->second;
}

bool KvStore::erase(const std::string_view key) {
    const std::unique_lock lock{mutex_};
    auto updated_entries = entries_;
    if (updated_entries.erase(std::string{key}) == 0U) {
        return false;
    }

    persistence_->save(updated_entries);
    entries_.swap(updated_entries);
    return true;
}

}  // namespace ftkv::storage
