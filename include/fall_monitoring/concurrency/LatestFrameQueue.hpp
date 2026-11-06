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
     * @brief 有界实时帧队列
     */
    template <typename T>
    class LatestFrameQueue final
    {
    public:
        explicit LatestFrameQueue(std::size_t capacity) : capacity_(capacity)
        {
            if (capacity == 0)
            {
                throw std::invalid_argument("LatestFrameQueue capacity 必须大于 0");
            }
        }

        LatestFrameQueue(const LatestFrameQueue&) = delete;
        LatestFrameQueue& operator=(const LatestFrameQueue&) = delete;

        /**
         * @brief 尝试写入最新数据
         * @return 已关闭时返回 false，其他情况成功入队
         * @note 传值允许生产者按需复制或移动；对象仅在持锁区间内入队
         */
        [[nodiscard]] bool push(T value)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_)
                {
                    return false;
                }
                if (queue_.size() == capacity_)
                {
                    queue_.pop_front();
                    ++droppedCount_;
                }
                queue_.push_back(std::move(value));
            }
            notEmpty_.notify_one();
            return true;
        }

        /**
         * @brief 超时等待帧，关闭时仍会取完队列中已有数据
         * @return 获取成功时为 true；超时、关闭且已为空为 false
         */
        [[nodiscard]] bool waitPopFor(T& out, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!notEmpty_.wait_for(lock, timeout, [this]{
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
            return true;
        }

        [[nodiscard]] bool tryPop(T& out)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty())
            {
                return false;
            }
            out = std::move(queue_.front());
            queue_.pop_front();
            return true;
        }

        /**
         * @brief 只负责状态切换和唤醒，由应用统一记录一次关闭日志
         */
        void close() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
            }
            notEmpty_.notify_all();
        }

        [[nodiscard]] std::size_t size() const 
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        /**
         * @brief 累计丢帧数，供视觉流水线定期输出 WARN/DEBUG 指标
         */
        [[nodiscard]] std::size_t droppedCount() const 
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return droppedCount_;
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
        std::deque<T> queue_;
        std::size_t droppedCount_ = 0;
        bool closed_ = false;
    };
}