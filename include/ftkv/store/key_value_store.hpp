#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ftkv::store {

// Defines the storage contract independently of networking and replication.
class KeyValueStore {
public:
    virtual ~KeyValueStore() = default;

    virtual void put(std::string key, std::string value) = 0;
    [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) const = 0;
    [[nodiscard]] virtual bool erase(std::string_view key) = 0;
};

}  // namespace ftkv::store
