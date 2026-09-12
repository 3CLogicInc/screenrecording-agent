#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "ipc/ipc_transport.hpp"

namespace screenrec::ipc {

// Per-connection registry shared by both server transports; `Native` is HANDLE / fd. All members mutex-guarded.
template <typename Native>
class ClientConnections {
public:
    ClientId add(Native handle, std::function<void(ClientId)> body) {
        const ClientId id = nextId_.fetch_add(1);
        std::scoped_lock lock(mutex_);
        handles_.emplace(id, handle);
        std::thread thread([this, id, body = std::move(body)]() {
            body(id);
            std::scoped_lock lock(mutex_);
            finished_.push_back(std::this_thread::get_id());
        });
        const auto tid = thread.get_id();
        threads_.emplace(tid, std::move(thread));
        return id;
    }

    [[nodiscard]] std::optional<Native> lookup(ClientId id) const {
        std::scoped_lock lock(mutex_);
        const auto it = handles_.find(id);
        if (it == handles_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void erase(ClientId id) {
        std::scoped_lock lock(mutex_);
        handles_.erase(id);
    }

    [[nodiscard]] std::vector<Native> handles() const {
        std::scoped_lock lock(mutex_);
        std::vector<Native> out;
        out.reserve(handles_.size());
        for (const auto& [id, handle] : handles_) {
            out.push_back(handle);
        }
        return out;
    }

    // Call periodically from the accept loop or thread objects accumulate.
    void reapFinished() {
        std::vector<std::thread> toJoin;
        {
            std::scoped_lock lock(mutex_);
            for (const auto tid : finished_) {
                if (const auto it = threads_.find(tid); it != threads_.end()) {
                    toJoin.push_back(std::move(it->second));
                    threads_.erase(it);
                }
            }
            finished_.clear();
        }
        for (auto& thread : toJoin) { // join outside the lock
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    // Call at stop(), after the readers have been woken.
    void joinAll() {
        std::map<std::thread::id, std::thread> threads;
        {
            std::scoped_lock lock(mutex_);
            threads = std::move(threads_);
            threads_.clear();
            finished_.clear();
        }
        for (auto& [tid, thread] : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    mutable std::mutex mutex_;
    std::map<ClientId, Native> handles_;
    std::map<std::thread::id, std::thread> threads_;
    std::vector<std::thread::id> finished_;
    std::atomic<ClientId> nextId_{1};
};

}
