#pragma once

#include "fall_monitoring/domain/event/AlertEvent.hpp"
#include "fall_monitoring/domain/vision/ISafeZoneQuery.hpp"
#include "fall_monitoring/domain/vision/PersonTracker.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace fall_monitoring::domain::vision
{
    /**
     * @brief 跌倒判定阈值；默认值保留旧类定义。
     * @note 正式运行时由 Application 从既有 config.json 的 rule_engine 节点覆盖。
     */
    struct FallRuleConfig
    {
        float kptConfThreshold = 0.3f;
        float fallAngleThreshold = 60.0f;
        float recoveryAngleThreshold = 35.0f;
        float normalizedVelocityThreshold = 0.6f;
        float normalizedCenterVelocityThreshold = 0.5f;
        int suspectConfirmMs = 1000;
        int staticLieConfirmMs = 5000;
        int fallEventWindowMs = 1500;
        float safeZoneOverlapThreshold = 0.5f;
    };

    enum class FallState { NORMAL, SUSPECTED_FALL, CONFIRMED_FALL, ALARMED };

    /**
     * @brief 每位人体独立的跌倒状态机；只消费姿态跟踪结果。
     *
     * 与旧版一致：角度+髋/框中心速度+快速下降窗口+持续躺卧兜底；
     * 支撑腿抑制弯腰误报，安全躺卧区域在提供查询实现后抑制报警；
     * ALARMED 状态下不重复报警，恢复直立后才允许下一次事件。
     *
     * 线程约束：每个实例只在 VisionPipeline 的推理线程中调用。
     */
    class FallRuleEngine final
    {
    public:
        explicit FallRuleEngine(FallRuleConfig config = {}, const ISafeZoneQuery* safeZoneQuery = nullptr);

        [[nodiscard]] std::vector<event::AlertEvent> processFrame(const std::vector<TrackedPerson>& trackedPersons, int frameWidth, int frameHeight);

    private:
        struct PersonState
        {
            FallState state = FallState::NORMAL;
            bool hasPreviousTarget = false;
            float previousHipY = 0.0f;
            float previousCenterY = 0.0f;
            float previousBodyHeight = 0.0f;
            std::chrono::steady_clock::time_point lastFrameTime{};
            std::chrono::steady_clock::time_point suspectedStartTime{};
            std::chrono::steady_clock::time_point lieStartTime{};
            std::chrono::steady_clock::time_point lastFastDropTime{};
            std::chrono::steady_clock::time_point lastSeenTime{};
            bool lieTimerActive = false;
            bool fastDropDetected = false;
        };

        bool processPerson(const TrackedPerson& person, int frameWidth, int frameHeight, PersonState& personState, event::AlertEvent& outEvent);
        void resetPersonToNormal(PersonState& personState);
        void removeExpiredPersonStates(std::chrono::steady_clock::time_point now);

        std::unordered_map<int, PersonState> personStates_;
        FallRuleConfig config_;
        const ISafeZoneQuery* safeZoneQuery_ = nullptr; // 非拥有指针，依赖由 Application 管理
    };
}
