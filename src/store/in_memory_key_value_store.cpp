#include "ftkv/store/in_memory_key_value_store.hpp"

#include <mutex>
#include <utility>

namespace ftkv::store {

void InMemoryKeyValueStore::put(std::string key, std::string value) {
    const std::unique_lock lock{mutex_};
    entries_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> InMemoryKeyValueStore::get(const std::string_view key) const {
    const std::shared_lock lock{mutex_};
    const auto entry = entries_.find(std::string{key});
    if (entry == entries_.end()) {
        return std::nullopt;
    }

    return entry->second;
}

bool InMemoryKeyValueStore::erase(const std::string_view key) {
    const std::unique_lock lock{mutex_};
    return entries_.erase(std::string{key}) != 0U;
}

} // namespace ftkv::store
