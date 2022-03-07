#pragma once

#include <coroutine>
#include <variant>
#include <concepts>
#include <cassert>

namespace co_context {

template<typename... F>
struct overloaded : F... {
    using F::operator()...;
};

template<typename... F>
overloaded(F...) -> overloaded<F...>;

template<typename T>
class task;

namespace detail {
    /**
     * @brief Define the behavior of all tasks.
     *
     * final_suspend: yes, and return to father
     */
    class task_promise_base {
        friend struct final_awaiter;

        /**
         * @brief current task is finished therefore resume the father
         */
        struct final_awaiter {
            constexpr bool await_ready() const noexcept { return false; }

            template<typename Promise>
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<Promise> current) noexcept {
                return current.promise().fa_coro;
            }

            // Won't be resumed anyway
            constexpr void await_resume() const noexcept {}
        };

      public:
        task_promise_base() noexcept = default;

        inline constexpr std::suspend_always initial_suspend() noexcept {
            return {};
        }

        inline constexpr final_awaiter final_suspend() noexcept { return {}; }

        inline void set_father(std::coroutine_handle<> continuation) noexcept {
            fa_coro = continuation;
        }

      private:
        std::coroutine_handle<> fa_coro;
    };

    /**
     * @brief Task with a return value
     *
     * @tparam T the type of the final result
     */
    template<typename T>
    class task_promise final : public task_promise_base {
      public:
        using var = std::variant<std::monostate, T, std::exception_ptr>;

      public:
        task_promise() noexcept = default;

        // Automatically deconstruct `value`, done by std::variant.
        ~task_promise() = default;

        inline task<T> get_return_object() noexcept;

        inline void unhandled_exception() noexcept {
            value.template emplace<std::exception_ptr>(
                std::current_exception());
        }

        template<typename Value>
            requires std::convertible_to<Value &&, T>
        inline void return_value(Value &&result) noexcept(
            std::is_nothrow_constructible_v<T, Value &&>) {
            value.template emplace<T>(std::forward<Value>(result));
        }

        // get the lvalue ref
        inline T &result() & {
            if (value.index() == 2) [[unlikely]]
                std::rethrow_exception(get<std::exception_ptr>(value));
            assert(value.index() == 1);
            return get<T>(value);
        }

        // get the prvalue
        inline T &&result() && {
            if (value.index() == 2) [[unlikely]]
                std::rethrow_exception(get<std::exception_ptr>(value));
            assert(value.index() == 1);
            return std::move(get<T>(value));
        }

      private:
        var value;
    };

    template<>
    class task_promise<void> final : public task_promise_base {
      public:
        task_promise() noexcept = default;

        inline task<void> get_return_object() noexcept;

        inline constexpr void return_void() noexcept {}

        inline void result() {
            if (this->exception_ptr)
                std::rethrow_exception(this->exception_ptr);
        }

      private:
        std::exception_ptr exception_ptr;
    };

    template<typename T>
    class task_promise<T &> final : public task_promise_base {
      public:
        task_promise() noexcept = default;

        task<T &> get_return_object() noexcept;

        inline void unhandled_exception() noexcept {
            this->exception_ptr = std::current_exception();
        }

        inline void return_value(T &result) noexcept {
            value = std::addressof(result);
        }

        inline T &result() {
            if (exception_ptr) [[unlikely]]
                std::rethrow_exception(exception_ptr);
            return *value;
        }

      private:
        T *value = nullptr;
        std::exception_ptr exception_ptr;
    };

} // namespace detail

template<typename T = void>
class [[nodiscard]] task {
  public:
    using promise_type = detail::task_promise<T>;
    using value_type = T;

  private:
    struct awaiter_base {
        std::coroutine_handle<promise_type> handle;

        awaiter_base(std::coroutine_handle<promise_type> current) noexcept
            : handle(current) {}

        inline bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> fa_coro) noexcept {
            handle.promise().set_father(fa_coro);
            return handle;
        }
    };

  public:
    task() noexcept = default;

    explicit task(std::coroutine_handle<promise_type> current) noexcept
        : handle(current) {}

    task(task &&other) noexcept : handle(other) { other.handle = nullptr; }

    // Ban copy
    task(const task &) = delete;
    task &operator=(const task &) = delete;

    task &operator=(task &&other) noexcept {
        if (this != std::addressof(other)) [[likely]] {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    // Free the promise object and coroutine parameters
    ~task() {
        if (handle) handle.destroy();
    }

    inline bool is_ready() const noexcept { return !handle || handle.done(); }

    /**
     * @brief wait for the task to complete, and get the ref of the result
     */
    auto operator co_await() const &noexcept {
        struct awaiter : awaiter_base {
            using awaiter_base::awaiter_base;

            decltype(auto) await_resume() {
                if (!this->handle) throw std::logic_error("broken_promise");

                return this->handle.promise().result();
            }
        };

        return awaiter{handle};
    }

    /**
     * @brief wait for the task to complete, and get the rvalue ref of the
     * result
     */
    auto operator co_await() const &&noexcept {
        struct awaiter : awaiter_base {
            using awaiter_base::awaiter_base;

            decltype(auto) await_resume() {
                if (!this->handle) throw std::logic_error("broken_promise");

                return std::move(this->handle.promise()).result();
            }
        };

        return awaiter{handle};
    }

    /**
     * @brief wait for the task to complete, but do not get the result
     */
    auto when_ready() const noexcept {
        struct awaiter : awaiter_base {
            using awaiter_base::awaiter_base;
            constexpr void await_resume() const noexcept {}
        };

        return awaiter{handle};
    }

  private:
    std::coroutine_handle<promise_type> handle;
};

namespace detail {
    template<typename T>
    inline task<T> task_promise<T>::get_return_object() noexcept {
        return task<T>{
            std::coroutine_handle<task_promise>::from_promise(*this)};
    }

    inline task<void> task_promise<void>::get_return_object() noexcept {
        return task<void>{
            std::coroutine_handle<task_promise>::from_promise(*this)};
    }

    template<typename T>
    inline task<T &> task_promise<T &>::get_return_object() noexcept {
        return task<T &>{
            std::coroutine_handle<task_promise>::from_promise(*this)};
    }

    template<typename T>
    struct remove_rvalue_reference {
        using type = T;
    };

    template<typename T>
    struct remove_rvalue_reference<T &&> {
        using type = T;
    };

    template<typename T>
    using remove_rvalue_reference_t = typename remove_rvalue_reference<T>::type;

    template<typename Awaiter>
    using get_awaiter_result_t =
        decltype(std::declval<Awaiter>().await_resume());
} // namespace detail

template<typename Awaiter>
auto make_task(Awaiter awaiter) -> task<
    detail::remove_rvalue_reference_t<detail::get_awaiter_result_t<Awaiter>>> {
    co_return co_await static_cast<Awaiter &&>(awaiter);
}

} // namespace co_context
