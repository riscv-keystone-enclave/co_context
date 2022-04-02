#include "co_context/co/semaphore.hpp"
#include "co_context/log/log.hpp"
#include <cassert>

namespace co_context {

semaphore::~semaphore() noexcept {
    assert(awaiting.load(std::memory_order_relaxed) == nullptr);
    assert(to_resume == nullptr);
}

bool semaphore::try_acquire() noexcept {
    T old_counter = counter.load(std::memory_order_relaxed);
    return old_counter > 0
           && counter.compare_exchange_strong(
               old_counter, old_counter - 1, std::memory_order_acquire,
               std::memory_order_relaxed);
}

void semaphore::release(T update) noexcept {
    // register semaphore-update event, to io_context(worker)
    using namespace co_context::detail;
    auto *worker = this_thread.worker;
    assert(
        this_thread.worker != nullptr
        && "semaphore::release() must run inside an io_context");
    log::d("semaphore %lx released\n", this);
    worker->submit(task_info::new_semaphore_release(this, update));
};

std::coroutine_handle<> semaphore::try_release() noexcept {
    acquire_awaiter *resume_head = to_resume;
    if (resume_head == nullptr) {
        auto *node = awaiting.exchange(nullptr, std::memory_order_acquire);
        if (node == nullptr) return nullptr; // no awaiting

        do {
            acquire_awaiter *tmp = node->next;
            node->next = resume_head;
            resume_head = node;
            node = tmp;
        } while (node != nullptr);
    }

    assert(resume_head != nullptr);

    to_resume = resume_head->next;
    return resume_head->handle;
}

void semaphore::acquire_awaiter::await_suspend(
    std::coroutine_handle<> current) noexcept {
    this->handle = current;
    log::d("suspending coro: %lx\n", this->handle.address());
    // acquire failed
    acquire_awaiter *old_head = sem.awaiting.load(std::memory_order_acquire);
    do {
        this->next = old_head;
    } while (!sem.awaiting.compare_exchange_weak(
        old_head, this, std::memory_order_release, std::memory_order_relaxed));
}

// semaphore::

} // namespace co_context
