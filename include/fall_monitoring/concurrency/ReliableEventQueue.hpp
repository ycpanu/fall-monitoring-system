#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace fall_monitoring::concurrency
{
    /**
     * @brief 不静默丢事件的有界队列，采用生产者背压机制
     */
    template <typename T>
    class ReliableEventQueue final 
    {
    public:
        explicit ReliableEventQueue(std::size_t capacity) : capacity_(capacity)
        {
            if (capacity == 0)
            {
                throw std::invalid_argument("ReliableEventQueue capacity 必须大于 0");
            }

            ReliableEventQueue(const ReliableEventQueue&) = delete;
            ReliableEventQueue& operator=(const ReliableEventQueue&) = delete;
        }

        /**
         * @brief 队列满时阻塞，知道可写入或队列关闭
         * @note 若不允许长时间阻塞视觉线程，使用 pushFor() 并处理返回 false
         */
        [[nodiscard]] bool push(T& value)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            notFull_wait(lock, [this]{
                return closed_ || queue_.size() < capacity_;
            });

            if (closed_) return false;
            queue_.emplace_back(std::move(value));
            lock.unlock();
            notEmpty_.notify_one();
            return true;
        }

        /**
         * @brief 非阻塞尝试写入
         * @return 满队列/已关闭时返回 false，传入对象保持未被移动
         */
        [[nodiscard]] bool tryPush(T& value)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_ || queue_.size() >= capacity_)
                {
                    return false;
                }
                queue_.emplace_back(std::move(value));
            }
            notEmpty_.notify_one();
            return true;
        }

        /**
         * @brief 最多等待 timeout 时间插入事件
         */
        [[nodiscard]] bool pushFor(T&& value, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!notFull_.wait_for(lock, timeout, [this]{
                return closed_ || queue_.size() < capacity_;
            }))
            {
                return false;
            }
            queue_.emplace_back(std::move(value));
            lock.unlock();
            notEmpty_.notify_one();
            return true;
        }

        /** @brief 关闭后继续取出剩余事件；关闭且为空才返回 false。 */
        [[nodiscard]] bool waitPop(T& out)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            notEmpty_.wait(lock, [this]{
                return closed_ || !queue_.empty();
            });

            if (queue_.empty())
            {
                return false;
            }

            out = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            notFull_.notify_one();
            return true;
        }

        [[nodiscard]] bool waitPopFor(T& out, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if(!notEmpty_.wait_for(lock, timeout, [this]{
                return closed_ || !queue_.empty();
            }))
            {
                return false;
            }

            if (queue_.empty())
            {
                return false;
            }
            out = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            notFull_.notify_one();
            return true;
        }

        /**
         * @brief 唤醒阻塞生产者与消费者
         */
        void close() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
            }
            notEmpty_.notify_all();
            notFull_.notify_all();
        }

        [[nodiscard]] std::size_t size() const 
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        [[nodiscard]] bool isClosed() const 
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return closed_;
        }

    private:
        const std::size_t capacity_;
        mutable std::mutex mutex_;
        std::condition_variable notEmpty_;
        std::condition_variable notFull_;
        std::deque<T> queue_;
        bool closed_ = false;
    };
}