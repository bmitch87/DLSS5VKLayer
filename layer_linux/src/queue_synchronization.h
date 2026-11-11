// Private DLSS queue synchronization. Composition state has a separate owner.
#pragma once
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace dlssnr {
template<class Queue> class QueueSynchronization {
    std::shared_mutex device_gate_;
    std::mutex metadata_;
    std::unordered_map<Queue, std::shared_ptr<std::mutex>> queues_;
    std::shared_ptr<std::mutex> queue_mutex(Queue queue) {
        std::lock_guard<std::mutex> lock(metadata_);
        auto& entry = queues_[queue];
        if (!entry) entry = std::make_shared<std::mutex>();
        return entry;
    }
public:
    // Own the device reader before the queue mutex. No GPU fence/model wait may
    // live inside this lease: it protects a host queue operation, not a frame.
    class QueueLease {
        std::shared_lock<std::shared_mutex> device_;
        std::shared_ptr<std::mutex> mutex_;
        std::unique_lock<std::mutex> queue_;
    public:
        QueueLease(QueueSynchronization& owner, Queue queue)
            : device_(owner.device_gate_), mutex_(owner.queue_mutex(queue)), queue_(*mutex_) {}
    };
    class IdleLease {
        std::unique_lock<std::shared_mutex> device_;
    public:
        explicit IdleLease(QueueSynchronization& owner) : device_(owner.device_gate_) {}
    };
};
} // namespace dlssnr
