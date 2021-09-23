// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#ifndef SPDLOG_HEADER_ONLY
#    include <spdlog/details/thread_pool.h>
#endif

#include <spdlog/common.h>
#include <cassert>

namespace spdlog {
namespace details {

SPDLOG_INLINE thread_pool::thread_pool(size_t q_max_items, size_t threads_n, std::function<void()> on_thread_start)
    : q_(q_max_items)
{
    if (threads_n == 0 || threads_n > 1000)
    {
        throw_spdlog_ex("spdlog::thread_pool(): invalid threads_n param (valid "
                        "range is 1-1000)");
    }
    for (size_t i = 0; i < threads_n; i++)
    {
        threads_.emplace_back([this, on_thread_start] {
            on_thread_start();
            this->thread_pool::worker_loop_();
        });
    }
}

SPDLOG_INLINE thread_pool::thread_pool(size_t q_max_items, size_t threads_n)
    : thread_pool(q_max_items, threads_n, [] {})
{}

// message all threads to terminate gracefully join them
SPDLOG_INLINE thread_pool::~thread_pool()
{
    SPDLOG_TRY
    {
        for (size_t i = 0; i < threads_.size(); i++)
        {
            post_async_msg_(async_msg(async_msg_type::terminate), async_overflow_policy::block);
        }

        for (auto &t : threads_)
        {
            t.join();
        }
    }
    SPDLOG_CATCH_STD
}

void SPDLOG_INLINE thread_pool::post_log(async_logger_ptr &&worker_ptr, const details::log_msg &msg, async_overflow_policy overflow_policy)
{
    // clang-format off
    /**
     * 需要明确传递 worker_ptr 指针的目的：
     *    主要是为了保存指向的对象（延长对象的生命周期），以便在异步消息出队之后可以使用同样的logger对象进行日志处理。
     * 所以可以使用赋值或者 move。比如 post_lot 中 worker_ptr 参数也可以使用值传递的方式。
     */

    /**
     * 明确两个概念：
     * 1. 指针传参时使用移动语义
     *    1.1 转移对象的所有权：原来的指针不再持有该对象的所有权，新的指针获得对象的专属所有权。比如这里的 worker_ptr 指针所有权的传递过程
     *
     * 2. 普通对象传参时使用移动语义
     *    2.1 调用对象的移动构造函数而非拷贝构造函数，转移所有权，原来的对象不能再被使用。比如这里的 async_m 对象使用移动语义传参
     **/

    /**
     * 右值引用传参和左值引用传参的目的：
     * 左值引用：如果使用左值引用传参，编译器就认为在这个函数中会修改这个变量的值，并且这个被修改的引用在函数调用结束后会发挥作用。左值引用可以认为只在函数内部使用指针，但是并不打算涉及其生命周期的管理，也不打算通过函数传参延长对象的生命周期。
     *
     * 右值引用：调用移动语义，发生所有权的转移，函数调用结束后不关注这个引用的值。用户的目的是需要在函数中保存对象的指针，延长对象的生命周期，以便在后续需要的时候继续使用这个指针和对象。
     */

    /**
     * 两个问题：
     * 1. post_log 接口中指针 worker_ptr 传参时为什么不使用左值引用？
     *    1.1 和目的不符
     *        1.1.1 使用左值引用的目的是在函数内部对该变量进行修改，并不打算保存该指针变量，并且修改结果在函数调用结束后要发挥作用（编译器限制）
     *
     *    1.2 使用方式不符
     *    1.2.1 调用 post_log 接口时使用 shared_from_this() 作为临时变量传入，临时变量不能作为非 const 的引用参数。所以有两种解决方案：
     *    1.2.1.1 post_log 使用 const 左值引用传参。但是问题是传入之后的常量值不能被修改，也就不能使用 std::move 转移所有权或者将该变量传入其他非常量引用
     *    1.2.1.2 使用右值引用传参。移交对象生命周期的管理权限，意味着需要转移该临时变量的所有权给其他用户使用
     *
     * 2. post_async_msg_ 接口中对象 async_m 传参时为什么不使用左值引用？
     *    2.1 可以使用左值引用
     *
     * 最终结论：
     *    1.1 临时变量不能作为非 const 的引用参数。如果需要将临时变量作为引用参数传入，可以考虑用右值引用（虽然可能无法达到最初的目的）。
     *    1.2 临时变量可以作为右值引用
     **/
    // clang-format on

    // worker_ptr.use_count() != 0;
    async_msg async_m(std::move(worker_ptr), async_msg_type::log, msg); // worker_ptr 的所有权发生了转移
    // worker_ptr.use_count() == 0;

    // async_m.worker_ptr.use_count() != 0;
    post_async_msg_(std::move(async_m), overflow_policy); // async_m 的所有权发生了转移
    // async_m.worker_ptr.use_count() == 0;
}

void SPDLOG_INLINE thread_pool::post_flush(async_logger_ptr &&worker_ptr, async_overflow_policy overflow_policy)
{
    // async_msg(std::move(worker_ptr) 是临时变量，不能作为非 const 的引用参数
    // 如果需要使用左值引用的方式传参，需要单独创建 async_msg(std::move(worker_ptr) 对象
    post_async_msg_(async_msg(std::move(worker_ptr), async_msg_type::flush), overflow_policy);
}

size_t SPDLOG_INLINE thread_pool::overrun_counter()
{
    return q_.overrun_counter();
}

size_t SPDLOG_INLINE thread_pool::queue_size()
{
    return q_.size();
}

void SPDLOG_INLINE thread_pool::post_async_msg_(async_msg &&new_msg, async_overflow_policy overflow_policy)
{
    if (overflow_policy == async_overflow_policy::block)
    {
        /**
         * 为什么这里没有加锁？
         *    因为在 mpmc 中的 enqueue 方法中已经进行了加锁操作
         */
        q_.enqueue(std::move(new_msg));
    }
    else
    {
        q_.enqueue_nowait(std::move(new_msg));
    }
}

void SPDLOG_INLINE thread_pool::worker_loop_()
{
    while (process_next_msg_()) {}
}

// process next message in the queue
// return true if this thread should still be active (while no terminate msg
// was received)
bool SPDLOG_INLINE thread_pool::process_next_msg_()
{
    async_msg incoming_async_msg;
    bool dequeued = q_.dequeue_for(incoming_async_msg, std::chrono::seconds(10));
    if (!dequeued)
    {
        return true;
    }

    switch (incoming_async_msg.msg_type)
    {
    case async_msg_type::log: {
        incoming_async_msg.worker_ptr->backend_sink_it_(incoming_async_msg);
        return true;
    }
    case async_msg_type::flush: {
        incoming_async_msg.worker_ptr->backend_flush_();
        return true;
    }

    case async_msg_type::terminate: {
        return false;
    }

    default: {
        assert(false);
    }
    }

    return true;
}

} // namespace details
} // namespace spdlog
