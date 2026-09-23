#include "raft/raft.h"

#include <utility>

namespace ftkv::raft {

InMemoryRaftStorage::InMemoryRaftStorage(PersistentState initial_state)
    : state_{std::move(initial_state)} {}

PersistentState InMemoryRaftStorage::load() const {
    const std::scoped_lock lock{mutex_};
    return state_;
}

void InMemoryRaftStorage::save(const PersistentState& state) {
    const std::scoped_lock lock{mutex_};
    state_ = state;
}

Term message_term(const Message& message) {
    return std::visit([](const auto& rpc) { return rpc.term; }, message.rpc);
}

} // namespace ftkv::raft
