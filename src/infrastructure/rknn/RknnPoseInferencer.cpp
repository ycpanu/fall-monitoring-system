#include "fall_monitoring/infrastructure/rknn/RknnPoseInferencer.hpp"

#include <rknn_api.h>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace fall_monitoring::infrastructure::rknn
{
    using domain::vision::DetectResult;
    using domain::vision::KeyPoint;

    struct RknnPoseInferencer::Impl final
    {
        explicit Impl(std::string path, float conf, float nms) : modelPath_(std::move(path)), confThreshold_(conf), nmsThreshold_(nms) {}

        ~Impl() { releaseContext(); }

        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;

        [[nodiscard]] bool init();
        [[nodiscard]] bool detect(const cv::Mat& frame, std::vector<DetectResult>& results);

    private:
        void releaseContext() noexcept;
        static std::vector<unsigned char> loadModelFile(const std::string& path);
        void nms(std::vector<DetectResult>& inputBoxes, std::vector<DetectResult>& outputBoxes);
        static float calculateIoU(const DetectResult& box1, const DetectResult& box2);

        std::string modelPath_;
        rknn_context ctx_ = 0;
        bool isInitialized_ = false;
        std::vector<rknn_tensor_attr> inputAttrs_;
        std::vector<rknn_tensor_attr> outputAttrs_;
        int numInput_ = 0;
        int numOutput_ = 0;
        unsigned int diagnosticFrameCount_ = 0;
        cv::Size inputSize_;
        cv::Mat resizedBgr_;
        cv::Mat letterboxedRgb_;
        int warmupCount_ = 0;
        int measuredCount_ = 0;
        std::array<double, 5> stageTotalMs_{};
        double minRknnMs_ = std::numeric_limits<double>::max();
        double maxRknnMs_ = 0.0;
        float confThreshold_;
        float nmsThreshold_;
        static constexpr int NUM_CLASSES = 1;
        static constexpr int NUM_KEYPOINTS = 17;
    };

    void RknnPoseInferencer::Impl::releaseContext() noexcept
    {
        if (ctx_ != 0)
        {
            rknn_destroy(ctx_);
            ctx_ = 0;
            SPDLOG_INFO("[RKNN] NPU 上下文已释放");
        }
        inputAttrs_.clear();
        outputAttrs_.clear();
        numInput_ = numOutput_ = 0;
        isInitialized_ = false;
    }

    std::vector<unsigned char> RknnPoseInferencer::Impl::loadModelFile(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
        {
            SPDLOG_ERROR("[RKNN] 无法打开模型文件: {}", path);
            return {};
        }
        const std::streamoff length = input.tellg();
        if (length <= 0 || length > static_cast<std::streamoff>(std::numeric_limits<int>::max()))
        {
            SPDLOG_ERROR("[RKNN] 模型文件大小无效: {}", path);
            return {};
        }
        std::vector<unsigned char> data(static_cast<std::size_t>(length));
        input.seekg(0, std::ios::beg);
        if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        {
            SPDLOG_ERROR("[RKNN] 读取模型文件失败: {}", path);
            return {};
        }
        return data;
    }

    bool RknnPoseInferencer::Impl::init()
    {
        if (isInitialized_) return true;
        releaseContext(); // 若前一次初始化中途失败，释放残留的 SDK 状态。

        SPDLOG_INFO("[RKNN] 开始加载模型: {}", modelPath_);
        auto data = loadModelFile(modelPath_);
        if (data.empty()) return false;

        int rc = rknn_init(&ctx_, data.data(), static_cast<unsigned int>(data.size()), 0, nullptr);
        if (rc < 0)
        {
            SPDLOG_ERROR("[RKNN] rknn_init 失败: {}", rc);
            releaseContext();
            return false;
        }
        data.clear();
        data.shrink_to_fit();

        rknn_sdk_version version{};
        rc = rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
        if (rc == 0)
            SPDLOG_INFO("[RKNN] Runtime={}, Driver={}", version.api_version, version.drv_version);

        rknn_input_output_num io{};
        rc = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
        if (rc < 0 || io.n_input != 1 || io.n_output == 0)
        {
            SPDLOG_ERROR("[RKNN] 输入/输出张量数量不符合当前 Pose 模型: rc={}, in={}, out={}", rc, io.n_input, io.n_output);
            releaseContext();
            return false;
        }
        numInput_ = static_cast<int>(io.n_input);
        numOutput_ = static_cast<int>(io.n_output);
        inputAttrs_.resize(numInput_);
        outputAttrs_.resize(numOutput_);
        for (int i = 0; i < numInput_; ++i)
        {
            inputAttrs_[i].index = i;
            rc = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &inputAttrs_[i], sizeof(rknn_tensor_attr));
            if (rc < 0)
            {
                SPDLOG_ERROR("[RKNN] 输入张量 {} 查询失败: {}", i, rc);
                releaseContext();
                return false;
            }
        }
        for (int i = 0; i < numOutput_; ++i)
        {
            outputAttrs_[i].index = i;
            rc = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &outputAttrs_[i], sizeof(rknn_tensor_attr));
            if (rc < 0)
            {
                SPDLOG_ERROR("[RKNN] 输出张量 {} 查询失败: {}", i, rc);
                releaseContext();
                return false;
            }
        }
        // 保留旧工程对单输入、四维 NHWC/NCHW、三通道的约束。
        const auto& attr = inputAttrs_[0];
        if (attr.n_dims != 4 || (attr.fmt != RKNN_TENSOR_NHWC && attr.fmt != RKNN_TENSOR_NCHW))
        {
            SPDLOG_ERROR("[RKNN] 需要四维 NHWC/NCHW 图像输入");
            releaseContext();
            return false;
        }
        const bool nhwc = attr.fmt == RKNN_TENSOR_NHWC;
        inputSize_ = cv::Size(attr.dims[nhwc ? 2 : 3], attr.dims[nhwc ? 1 : 2]);
        const int channels = attr.dims[nhwc ? 3 : 1];
        if (attr.dims[0] != 1 || channels != 3 || inputSize_.width <= 0 || inputSize_.height <= 0)
        {
            SPDLOG_ERROR("[RKNN] 输入图像 batch/channel/尺寸不符合要求");
            releaseContext();
            return false;
        }
        // 保留旧工程解码假设：[1, 56, anchors] 的连续 float 输出。
        constexpr int outputAttributes = 4 + NUM_CLASSES + NUM_KEYPOINTS * 3;
        if (outputAttrs_[0].n_elems == 0 || outputAttrs_[0].n_elems % outputAttributes != 0)
        {
            SPDLOG_ERROR("[RKNN] Pose 输出张量元素数量无效: {}", outputAttrs_[0].n_elems);
            releaseContext();
            return false;
        }
        SPDLOG_INFO("[RKNN] 模型初始化完成: 输入={}x{}, 输出张量={}, anchors={}", inputSize_.width, inputSize_.height, numOutput_, outputAttrs_[0].n_elems / outputAttributes);
        isInitialized_ = true;
        return true;
    }
        bool RknnPoseInferencer::Impl::detect(const cv::Mat& frame, std::vector<DetectResult>& results)
        {
            results.clear(); // 失败时不能保留上一帧的人体检测结果。
            constexpr int WARMUP_FRAMES = 20;
            constexpr int REPORT_FRAMES = 100;

            if (!isInitialized_)
            {
                SPDLOG_ERROR("调用 detect 前必须先成功执行 init()！");
                return false;
            }
            if (frame.empty())
            {
                SPDLOG_WARN("detect 收到空帧，跳过本次推理");
                return false;
            }

            const auto preprocessStart = std::chrono::steady_clock::now();
            const int reqWidth = inputSize_.width;
            const int reqHeight = inputSize_.height;

            float scale = std::min(static_cast<float>(reqWidth) / frame.cols, static_cast<float>(reqHeight) / frame.rows);
            int newW = static_cast<int>(std::round(frame.cols * scale));
            int newH = static_cast<int>(std::round(frame.rows * scale));
            int padW = (reqWidth - newW) / 2;
            int padH = (reqHeight - newH) / 2;

            // 消除魔法数字，定义为明确的常量
            constexpr int LETTERBOX_PAD_COLOR = 114;

            // 先缩放再交换通道，避免对原始大图转色；复用缓冲区并直接写入目标 ROI。
            cv::resize(frame, resizedBgr_, cv::Size(newW, newH));
            letterboxedRgb_.create(reqHeight, reqWidth, CV_8UC3);
            letterboxedRgb_.setTo(cv::Scalar::all(LETTERBOX_PAD_COLOR));
            cv::Mat imageRegion = letterboxedRgb_(cv::Rect(padW, padH, newW, newH));
            cv::cvtColor(resizedBgr_, imageRegion, cv::COLOR_BGR2RGB);

            rknn_input inputs[1];
            memset(inputs, 0, sizeof(inputs));
            inputs[0].index = 0;
            inputs[0].type = RKNN_TENSOR_UINT8;     
            inputs[0].size = letterboxedRgb_.total() * letterboxedRgb_.elemSize();
            inputs[0].fmt = RKNN_TENSOR_NHWC;       
            inputs[0].buf = letterboxedRgb_.data;
            inputs[0].pass_through = 0;

            const auto preprocessEnd = std::chrono::steady_clock::now();
            int ret = rknn_inputs_set(ctx_, numInput_, inputs);
            if (ret < 0)
            {
                SPDLOG_ERROR("rknn_inputs_set 失败！错误码：{}", ret);
                return false;
            }

            auto rknnStart = std::chrono::steady_clock::now();

            ret = rknn_run(ctx_, nullptr);

            auto rknnEnd = std::chrono::steady_clock::now();

            if (ret < 0)
            {
                SPDLOG_ERROR("rknn_run 推理失败！错误码：{}", ret);
                return false;
            }

            double rknnMs = std::chrono::duration<double, std::milli>(rknnEnd - rknnStart).count();

            // 修复 VLA 变长数组问题，使用标准的 std::vector 容器
            std::vector<rknn_output> outputs(numOutput_);
            memset(outputs.data(), 0, sizeof(rknn_output) * numOutput_);
            for (int i = 0; i < numOutput_; i++)
            {
                outputs[i].index = i;
                // Runtime 将量化输出转换为 float，下面按 float* 解析，无需再次反量化。
                outputs[i].want_float = 1;      
            }

            // 传入 .data() 获取底层指针
            ret = rknn_outputs_get(ctx_, numOutput_, outputs.data(), nullptr);
            const auto outputEnd = std::chrono::steady_clock::now();
            if (ret < 0)
            {
                SPDLOG_ERROR("rknn_outputs_get 失败！错误码：{}", ret);
                return false;
            }

            // 输出缓冲区来自 RKNN Runtime。保证后处理出现异常也会释放。
            struct OutputLease
            {
                rknn_context ctx;
                int count;
                std::vector<rknn_output>& values;
                bool active = true;
                ~OutputLease() { if (active) rknn_outputs_release(ctx, count, values.data()); }
                void release()
                {
                    if (!active) return;
                    rknn_outputs_release(ctx, count, values.data());
                    active = false;
                }
            } outputLease{ctx_, numOutput_, outputs};
            if (!outputs[0].buf)
            {
                SPDLOG_ERROR("[RKNN] 输出张量缓冲区为空");
                return false;
            }
            float* outData = static_cast<float*>(outputs[0].buf);
            std::vector<DetectResult> candidates;

            const int KPT_OFFSET = 4 + NUM_CLASSES;                 
            const int TOTAL_ATTR = KPT_OFFSET + NUM_KEYPOINTS * 3;  
            const int NUM_ANCHORS = static_cast<int>(outputAttrs_[0].n_elems) / TOTAL_ATTR;
            float maxPersonConfidence = -std::numeric_limits<float>::infinity();
            int nonFiniteScores = 0;

            for (int i = 0; i < NUM_ANCHORS; i++)
            {
                float clsConf = outData[4 * NUM_ANCHORS + i];
                if (std::isfinite(clsConf))
                {
                    maxPersonConfidence = std::max(maxPersonConfidence, clsConf);
                }
                else
                {
                    ++nonFiniteScores;
                    continue;
                }
                if (clsConf <= confThreshold_) continue;

                float cx = outData[0 * NUM_ANCHORS + i];
                float cy = outData[1 * NUM_ANCHORS + i];
                float w  = outData[2 * NUM_ANCHORS + i];
                float h  = outData[3 * NUM_ANCHORS + i];

                DetectResult box;
                box.classId = 0;   
                box.confidence = clsConf;
                box.x = static_cast<int>((cx - w / 2.0f - padW) / scale);
                box.y = static_cast<int>((cy - h / 2.0f - padH) / scale);
                box.width = static_cast<int>(w / scale);
                box.height = static_cast<int>(h / scale);

                box.keypoints.reserve(NUM_KEYPOINTS);
                for (int k = 0; k < NUM_KEYPOINTS; k++)
                {
                    KeyPoint kp;
                    kp.x = static_cast<int>((outData[(KPT_OFFSET + k * 3 + 0) * NUM_ANCHORS + i] - padW) / scale);
                    kp.y = static_cast<int>((outData[(KPT_OFFSET + k * 3 + 1) * NUM_ANCHORS + i] - padH) / scale);
                    kp.confidence = outData[(KPT_OFFSET + k * 3 + 2) * NUM_ANCHORS + i];
                    box.keypoints.push_back(kp);
                }

                candidates.push_back(box);
            }

            nms(candidates, results);

            // INFO 级别只打印前 3 帧及每 100 帧，便于与 ONNX 的 person_scores.max() 对比。
            ++diagnosticFrameCount_;
            if (diagnosticFrameCount_ <= 3 || diagnosticFrameCount_ % 100 == 0)
            {
                SPDLOG_INFO("[POSE诊断] frame={}, input={}x{}, want_float=1, output_bytes={}, max_person_conf={}, non_finite_scores={}, threshold={}, candidates={}, persons={}",
                         diagnosticFrameCount_, reqWidth, reqHeight, outputs[0].size,
                         maxPersonConfidence, nonFiniteScores, confThreshold_, candidates.size(), results.size());
            }

            if (results.empty())
            {
                SPDLOG_TRACE("本帧未解析出有效人体（候选框 {} 个，confThreshold={}）", candidates.size(), confThreshold_);
            }
            else
            {
                SPDLOG_TRACE("本帧解析出 {} 个人体，第一个含 {} 个关键点", results.size(), results[0].keypoints.size());
            }

            // 传入 .data() 获取底层指针
            outputLease.release();

            const auto postprocessEnd = std::chrono::steady_clock::now();
            if (warmupCount_ < WARMUP_FRAMES)
            {
                ++warmupCount_;
            }
            else
            {
                const auto milliseconds = [](auto duration) {
                    return std::chrono::duration<double, std::milli>(duration).count();
                };
                const std::array<double, 5> stages = {
                    milliseconds(preprocessEnd - preprocessStart),
                    milliseconds(rknnStart - preprocessEnd),
                    rknnMs,
                    milliseconds(outputEnd - rknnEnd),
                    milliseconds(postprocessEnd - outputEnd)
                };
                for (size_t i = 0; i < stages.size(); ++i) stageTotalMs_[i] += stages[i];
                minRknnMs_ = std::min(minRknnMs_, rknnMs);
                maxRknnMs_ = std::max(maxRknnMs_, rknnMs);
                if (++measuredCount_ >= REPORT_FRAMES)
                {
                    SPDLOG_INFO("[PERF] RKNN rknn_run: avg={:.2f} ms, min={:.2f} ms, max={:.2f} ms, samples={}",
                             stageTotalMs_[2] / measuredCount_, minRknnMs_, maxRknnMs_, measuredCount_);
                    SPDLOG_INFO("[PERF] RKNN stages: preprocess={:.2f} ms, inputs_set={:.2f} ms, run={:.2f} ms, outputs_get={:.2f} ms, postprocess={:.2f} ms, samples={}",
                             stageTotalMs_[0] / measuredCount_, stageTotalMs_[1] / measuredCount_,
                             stageTotalMs_[2] / measuredCount_, stageTotalMs_[3] / measuredCount_,
                             stageTotalMs_[4] / measuredCount_, measuredCount_);
                    measuredCount_ = 0;
                    stageTotalMs_.fill(0.0);
                    minRknnMs_ = std::numeric_limits<double>::max();
                    maxRknnMs_ = 0.0;
                }
            }

            return true;
        }

        void RknnPoseInferencer::Impl::nms(std::vector<DetectResult>& inputBoxes, std::vector<DetectResult>& outputBoxes)
        {
            outputBoxes.clear();

            std::sort(inputBoxes.begin(), inputBoxes.end(), [](const DetectResult& a, const DetectResult& b)
            {
                return a.confidence > b.confidence;
            });

            std::vector<bool> isSuppressed(inputBoxes.size(), false);

            for (size_t i = 0; i < inputBoxes.size(); i++)
            {
                if (isSuppressed[i]) continue;

                outputBoxes.push_back(inputBoxes[i]);   

                for (size_t j = i + 1; j < inputBoxes.size(); j++)
                {
                    if (!isSuppressed[j] && inputBoxes[i].classId == inputBoxes[j].classId)
                    {
                        float iou = calculateIoU(inputBoxes[i], inputBoxes[j]);
                        if (iou > nmsThreshold_)
                        {
                            isSuppressed[j] = true;
                        }
                    }
                }
            }
        }

        float RknnPoseInferencer::Impl::calculateIoU(const DetectResult& box1, const DetectResult& box2)
        {
            int x1 = std::max(box1.x, box2.x);
            int y1 = std::max(box1.y, box2.y);
            int x2 = std::min(box1.x + box1.width, box2.x + box2.width);
            int y2 = std::min(box1.y + box1.height, box2.y + box2.height);

            if (x1 >= x2 || y1 >= y2) return 0.0f;

            float intersectionArea = static_cast<float>((x2 - x1) * (y2 - y1));
            float box1Area = static_cast<float>(box1.width * box1.height);
            float box2Area = static_cast<float>(box2.width * box2.height);

            float unionArea = box1Area + box2Area - intersectionArea;
            if (unionArea <= 0.0f) return 0.0f;

            return intersectionArea / unionArea;
        }

    RknnPoseInferencer::RknnPoseInferencer(std::string modelPath, float confidenceThreshold, float nmsThreshold)
        : impl_(std::make_unique<Impl>(std::move(modelPath), confidenceThreshold, nmsThreshold))
    {
    }

    RknnPoseInferencer::~RknnPoseInferencer() = default;

    bool RknnPoseInferencer::initialize()
    {
        return impl_->init();
    }

    bool RknnPoseInferencer::infer(const cv::Mat& frame, std::vector<domain::vision::DetectResult>& results)
    {
        return impl_->detect(frame, results);
    }
}
