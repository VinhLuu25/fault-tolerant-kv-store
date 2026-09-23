#include "ftkv/store/in_memory_key_value_store.hpp"

#include <iostream>

int main() {
    // Networking will be attached here once the transport layer is introduced.
    ftkv::store::InMemoryKeyValueStore store;
    store.put("status", "ready");
    std::cout << "ftkv_server foundation " << store.get("status").value_or("unavailable") << '\n';
    return 0;
}
