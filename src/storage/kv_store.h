#pragma once

#include "ftkv/store/key_value_store.hpp"

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ftkv::storage {

using EntryMap = std::unordered_map<std::string, std::string>;

class StorageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Separates storage semantics from the durable representation.
class Persistence {
public:
    virtual ~Persistence() = default;

    [[nodiscard]] virtual EntryMap load() const = 0;
    virtual void save(const EntryMap& entries) const = 0;
};

// Stores a complete, versioned snapshot and replaces the previous file atomically.
class FilePersistence final : public Persistence {
public:
    explicit FilePersistence(std::filesystem::path file_path);

    [[nodiscard]] EntryMap load() const override;
    void save(const EntryMap& entries) const override;

private:
    std::filesystem::path file_path_;
};

class KvStore final : public store::KeyValueStore {
public:
    explicit KvStore(std::unique_ptr<Persistence> persistence);

    void put(std::string key, std::string value) override;
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
    [[nodiscard]] bool erase(std::string_view key) override;

private:
    std::unique_ptr<Persistence> persistence_;
    mutable std::shared_mutex mutex_;
    EntryMap entries_;
};

}  // namespace ftkv::storage
