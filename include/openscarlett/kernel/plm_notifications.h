#pragma once

// Thread-safe Xbox Game Core (appnotify.h) Process Lifecycle Management (PLM)
// notification registry. Used by kernelx.dll and xgameruntime.dll to export
// RegisterAppStateChangeNotification / UnregisterAppStateChangeNotification /
// RegisterAppConstrainedChangeNotification / UnregisterAppConstrainedChangeNotification.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace openscarlett {

using PlmStateChangeCallback = void(__stdcall*)(uint8_t quiesced, void* context);
using PlmConstrainedChangeCallback = void(__stdcall*)(uint8_t constrained, void* context);

class PlmNotificationRegistry {
public:
    static constexpr uint32_t kErrorSuccess = 0u;
    static constexpr uint32_t kErrorInvalidParameter = 87u;

    uint32_t RegisterStateChange(
        PlmStateChangeCallback routine,
        void* context,
        void** out_registration) {
        if (!routine || !out_registration) {
            return kErrorInvalidParameter;
        }
        *out_registration = nullptr;
        const uint64_t handle = next_handle_.fetch_add(1ull, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            state_entries_.push_back(StateEntry{handle, routine, context});
        }
        *out_registration = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
        return kErrorSuccess;
    }

    bool UnregisterStateChange(void* registration) {
        if (!registration) return false;
        const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(registration));
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = state_entries_.begin(); it != state_entries_.end(); ++it) {
            if (it->handle == handle) {
                state_entries_.erase(it);
                return true;
            }
        }
        return false;
    }

    uint32_t RegisterConstrainedChange(
        PlmConstrainedChangeCallback routine,
        void* context,
        void** out_registration) {
        if (!routine || !out_registration) {
            return kErrorInvalidParameter;
        }
        *out_registration = nullptr;
        const uint64_t handle = next_handle_.fetch_add(1ull, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            constrained_entries_.push_back(ConstrainedEntry{handle, routine, context});
        }
        *out_registration = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
        return kErrorSuccess;
    }

    bool UnregisterConstrainedChange(void* registration) {
        if (!registration) return false;
        const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(registration));
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = constrained_entries_.begin(); it != constrained_entries_.end(); ++it) {
            if (it->handle == handle) {
                constrained_entries_.erase(it);
                return true;
            }
        }
        return false;
    }

    uint32_t DispatchStateChange(uint8_t quiesced) {
        std::vector<StateEntry> snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            snapshot = state_entries_;
        }
        for (const auto& entry : snapshot) {
            if (entry.routine) {
                entry.routine(quiesced, entry.context);
            }
        }
        return static_cast<uint32_t>(snapshot.size());
    }

    uint32_t DispatchConstrainedChange(uint8_t constrained) {
        std::vector<ConstrainedEntry> snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            snapshot = constrained_entries_;
        }
        for (const auto& entry : snapshot) {
            if (entry.routine) {
                entry.routine(constrained, entry.context);
            }
        }
        return static_cast<uint32_t>(snapshot.size());
    }

    void GetCounts(uint32_t* out_state_count, uint32_t* out_constrained_count) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (out_state_count) *out_state_count = static_cast<uint32_t>(state_entries_.size());
        if (out_constrained_count) *out_constrained_count = static_cast<uint32_t>(constrained_entries_.size());
    }

    void ClearAll() {
        std::lock_guard<std::mutex> lk(mutex_);
        state_entries_.clear();
        constrained_entries_.clear();
    }

private:
    struct StateEntry {
        uint64_t handle = 0ull;
        PlmStateChangeCallback routine = nullptr;
        void* context = nullptr;
    };
    struct ConstrainedEntry {
        uint64_t handle = 0ull;
        PlmConstrainedChangeCallback routine = nullptr;
        void* context = nullptr;
    };

    mutable std::mutex mutex_;
    std::atomic<uint64_t> next_handle_{0x504C4D0000000001ull};
    std::vector<StateEntry> state_entries_;
    std::vector<ConstrainedEntry> constrained_entries_;
};

}  // namespace openscarlett
