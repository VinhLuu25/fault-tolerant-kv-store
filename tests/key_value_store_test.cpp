#include "ftkv/store/in_memory_key_value_store.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    ftkv::store::InMemoryKeyValueStore store;

    expect(!store.get("missing").has_value(), "a missing key must not return a value");

    store.put("region", "ap-southeast-1");
    expect(store.get("region") == "ap-southeast-1", "put must make a value readable");

    store.put("region", "eu-west-1");
    expect(store.get("region") == "eu-west-1", "put must replace an existing value");

    expect(store.erase("region"), "erase must report an existing key");
    expect(!store.erase("region"), "erase must report a missing key");

    return EXIT_SUCCESS;
}
