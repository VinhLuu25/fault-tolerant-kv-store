#include "storage/kv_store.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ftkv::storage::EntryMap;
using ftkv::storage::FilePersistence;
using ftkv::storage::KvStore;
using ftkv::storage::Persistence;
using ftkv::storage::StorageError;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Operation>
void expect_throws(Operation&& operation, const std::string_view message) {
    try {
        std::forward<Operation>(operation)();
    } catch (const Exception&) {
        return;
    } catch (...) {
        std::cerr << "FAILED: " << message << " (unexpected exception type)\n";
        std::exit(EXIT_FAILURE);
    }

    std::cerr << "FAILED: " << message << " (no exception)\n";
    std::exit(EXIT_FAILURE);
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto unique_id = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ftkv-storage-test-" + std::to_string(unique_id));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error{"failed to create temporary test directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class MemoryPersistence final : public Persistence {
  public:
    [[nodiscard]] EntryMap load() const override { return entries_; }

    void save(const EntryMap& entries) const override {
        if (fail_saves) {
            throw StorageError{"injected persistence failure"};
        }
        entries_ = entries;
        ++save_count;
    }

    mutable EntryMap entries_;
    mutable std::size_t save_count{0};
    std::atomic_bool fail_saves{false};
};

void test_file_persistence_and_crud() {
    const TemporaryDirectory directory;
    const auto storage_file = directory.path() / "nested" / "store.data";
    const std::string binary_value{"value\0with\0bytes", 16};

    {
        KvStore store{std::make_unique<FilePersistence>(storage_file)};
        expect(!store.get("missing").has_value(), "GET must distinguish a missing key");

        store.put("key", "first");
        expect(store.get("key") == "first", "PUT must make a value readable");

        store.put("key", binary_value);
        expect(store.get("key") == binary_value, "PUT must replace and preserve binary values");
        expect(!store.erase("missing"), "DELETE must report a missing key");

        store.put("durable", "saved");
        expect(store.erase("key"), "DELETE must remove an existing key");
    }

    const KvStore reopened{std::make_unique<FilePersistence>(storage_file)};
    expect(reopened.get("durable") == "saved", "data must survive reopening the store");
    expect(!reopened.get("key").has_value(), "deletions must survive reopening the store");
}

void test_corrupt_file_error() {
    const TemporaryDirectory directory;
    const auto storage_file = directory.path() / "corrupt.data";
    {
        std::ofstream output{storage_file, std::ios::binary};
        output << "not-a-valid-snapshot";
    }

    expect_throws<StorageError>(
        [&storage_file] { KvStore store{std::make_unique<FilePersistence>(storage_file)}; },
        "opening a corrupt snapshot must fail");
}

void test_invalid_configuration() {
    expect_throws<std::invalid_argument>(
        [] { static_cast<void>(FilePersistence{std::filesystem::path{}}); },
        "an empty storage path must be rejected");
    expect_throws<std::invalid_argument>([] { KvStore store{std::unique_ptr<Persistence>{}}; },
                                         "a missing persistence implementation must be rejected");
}

void test_failed_write_rolls_back() {
    auto persistence = std::make_unique<MemoryPersistence>();
    auto* persistence_handle = persistence.get();
    KvStore store{std::move(persistence)};
    store.put("key", "committed");

    persistence_handle->fail_saves = true;
    expect_throws<StorageError>([&store] { store.put("key", "uncommitted"); },
                                "PUT must report persistence failures");
    expect(store.get("key") == "committed", "failed PUT must preserve committed state");

    expect_throws<StorageError>([&store] { static_cast<void>(store.erase("key")); },
                                "DELETE must report persistence failures");
    expect(store.get("key") == "committed", "failed DELETE must preserve committed state");
}

void test_concurrent_access() {
    constexpr int kThreadCount = 8;
    constexpr int kWritesPerThread = 100;
    auto persistence = std::make_unique<MemoryPersistence>();
    auto* persistence_handle = persistence.get();
    KvStore store{std::move(persistence)};

    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (int thread = 0; thread < kThreadCount; ++thread) {
        workers.emplace_back([thread, &store] {
            for (int write = 0; write < kWritesPerThread; ++write) {
                const auto key = std::to_string(thread) + ":" + std::to_string(write);
                store.put(key, "value");
                expect(store.get(key) == "value", "concurrent GET must observe a completed PUT");
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    expect(persistence_handle->save_count ==
               static_cast<std::size_t>(kThreadCount * kWritesPerThread),
           "each successful concurrent PUT must be persisted");
    expect(persistence_handle->entries_.size() ==
               static_cast<std::size_t>(kThreadCount * kWritesPerThread),
           "concurrent PUT operations must not lose entries");
}

} // namespace

int main() {
    test_file_persistence_and_crud();
    test_corrupt_file_error();
    test_invalid_configuration();
    test_failed_write_rolls_back();
    test_concurrent_access();
    return EXIT_SUCCESS;
}
