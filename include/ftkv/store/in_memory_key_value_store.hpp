#pragma once

#include "ftkv/store/key_value_store.hpp"

#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace ftkv::store {

// A thread-safe, non-durable implementation used as the initial storage engine.
class InMemoryKeyValueStore final : public KeyValueStore {
public:
    void put(std::string key, std::string value) override;
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
    [[nodiscard]] bool erase(std::string_view key) override;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> entries_;
};

}  // namespace ftkv::store
