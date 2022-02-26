// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/details/mpmc_blocking_q.h>
#include <spdlog/details/os.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <functional>

namespace spdlog {
class async_logger;

namespace details {

using async_logger_ptr = std::shared_ptr<spdlog::async_logger>;

enum class async_msg_type
{
    log,
    flush,
    terminate
};

// Async msg to move to/from the queue
// Movable only. should never be copied
struct async_msg : log_msg_buffer
{
    async_msg_type msg_type{async_msg_type::log};
    async_logger_ptr worker_ptr;

    async_msg() = default;
    ~async_msg() = default;

    // should only be moved in or out of the queue..
    async_msg(const async_msg &) = delete;

// support for vs2013 move
#if defined(_MSC_VER) && _MSC_VER <= 1800
    async_msg(async_msg &&other)
        : log_msg_buffer(std::move(other))
        , msg_type(other.msg_type)
        , worker_ptr(std::move(other.worker_ptr))
    {}

    async_msg &operator=(async_msg &&other)
    {
        *static_cast<log_msg_buffer *>(this) = std::move(other);
        msg_type = other.msg_type;
        worker_ptr = std::move(other.worker_ptr);
        return *this;
    }
#else // (_MSC_VER) && _MSC_VER <= 1800
    async_msg(async_msg &&) = default;
    async_msg &operator=(async_msg &&) = default;
#endif

    // construct from log_msg with given type
    async_msg(async_logger_ptr &&worker, async_msg_type the_type, const details::log_msg &m)
        : log_msg_buffer{m}
        , msg_type{the_type}
        , worker_ptr{std::move(worker)}
    {}

    async_msg(async_logger_ptr &&worker, async_msg_type the_type)
        : log_msg_buffer{}
        , msg_type{the_type}
        , worker_ptr{std::move(worker)}
    {}

    explicit async_msg(async_msg_type the_type)
        : async_msg{nullptr, the_type}
    {}
};

// RAII 手法封装的 thread。marked by jinglong in 2021年9月27日09:49:33
// 需要单独注意的是，std::thread 的拷贝构造和拷贝赋值被 delete 了，所以不可以用拷贝的方式传递线程对象
class SPDLOG_API thread_guard
{
public:
    // 使用引用传参，不用在构造函数中判断线程是否有效
    explicit thread_guard(std::thread &t)
        : t_(t)
    {}

    ~thread_guard()
    {
        if (t_.joinable())
        {
            t_.join();
        }
    }

    thread_guard(const thread_guard &) = delete;
    thread_guard &operator=(const thread_guard &) = delete;

private:
    std::thread &t_;
};

// RAII 手法封装的 thread。marked by jinglong in 2021年9月27日09:49:33
class SPDLOG_API scoped_thread
{
public:
    // 使用的是移动语义，所以必须在构造函数中判断该线程是否有效
    explicit scoped_thread(std::thread t)
        : t_(std::move(t))
    {
        if (!t_.joinable())
        {
            throw std::logic_error("No thread");
        }
    }

    ~scoped_thread()
    {
        t_.joinable();
    }

    scoped_thread(const scoped_thread &) = delete;
    scoped_thread &operator=(const scoped_thread &) = delete;

private:
    std::thread t_;
};

class SPDLOG_API thread_pool
{
public:
    using item_type = async_msg;
    using q_type = details::mpmc_blocking_queue<item_type>;

    thread_pool(size_t q_max_items, size_t threads_n, std::function<void()> on_thread_start);
    thread_pool(size_t q_max_items, size_t threads_n);

    // message all threads to terminate gracefully join them
    ~thread_pool();

    thread_pool(const thread_pool &) = delete;
    thread_pool &operator=(thread_pool &&) = delete;

    void post_log(async_logger_ptr &&worker_ptr, const details::log_msg &msg, async_overflow_policy overflow_policy);
    void post_flush(async_logger_ptr &&worker_ptr, async_overflow_policy overflow_policy);
    size_t overrun_counter();
    size_t queue_size();

private:
    q_type q_;

    std::vector<std::thread> threads_;

    void post_async_msg_(async_msg &&new_msg, async_overflow_policy overflow_policy);
    void worker_loop_();

    // process next message in the queue
    // return true if this thread should still be active (while no terminate msg
    // was received)
    bool process_next_msg_();
};

} // namespace details
} // namespace spdlog

#ifdef SPDLOG_HEADER_ONLY
#    include "thread_pool-inl.h"
#endif
