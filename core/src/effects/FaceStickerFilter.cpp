#include "effects/FaceStickerFilter.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace PLAYER {
FaceStickerFilter::~FaceStickerFilter()
{
    if (m_session) HFReleaseInspireFaceSession(m_session);
}
bool FaceStickerFilter::Initialize(const std::string& modelPath)
{
    if (m_session) return true;
    // SDK 全局模型由进程持有，session 只由检测工作线程使用。
    static std::mutex sc_launch_mutex;
    static bool sc_launched = false;
    std::lock_guard<std::mutex> lock(sc_launch_mutex);
    if (!sc_launched) {
        const auto ret = HFLaunchInspireFace(modelPath.c_str());
        if (ret != HSUCCEED) { m_error = "人脸模型加载失败: " + std::to_string(ret); return false; }
        sc_launched = true;
    }
    // 每次检测独立输入，适应抽帧、跳转和场景切换，避免沿用旧跟踪状态。
    const auto ret = HFCreateInspireFaceSessionOptional(0, HF_DETECT_MODE_ALWAYS_DETECT, 8, 320, -1, &m_session);
    if (ret != HSUCCEED) { m_error = "创建人脸检测器失败: " + std::to_string(ret); return false; }
    return true;
}
bool FaceStickerFilter::Detect(const AVFrame& frame, std::vector<FaceStickerPose>& poses)
{
    poses.clear();
    if (!m_session || frame.format != AV_PIX_FMT_RGBA || frame.width <= 0 || frame.height <= 0 || !frame.data[0]) return false;
    const size_t stride = static_cast<size_t>(frame.width) * 4;
    if (std::abs(static_cast<int64_t>(frame.linesize[0])) < static_cast<int64_t>(stride)) return false;
    m_pixels.resize(stride * frame.height);
    for (int y = 0; y < frame.height; ++y)
        std::memcpy(m_pixels.data() + y * stride, frame.data[0] + static_cast<ptrdiff_t>(y) * frame.linesize[0], stride);
    HFImageData image{m_pixels.data(), frame.width, frame.height, HF_STREAM_RGBA, HF_CAMERA_ROTATION_0};
    HFImageStream stream = nullptr;
    auto ret = HFCreateImageStream(&image, &stream);
    if (ret != HSUCCEED) { m_error = "创建检测图像失败: " + std::to_string(ret); return false; }
    // poses 扩容等操作可能抛异常，图像句柄仍必须归还 SDK。
    const auto releaseStream = [](auto* value) { HFReleaseImageStream(value); };
    std::unique_ptr<std::remove_pointer_t<HFImageStream>, decltype(releaseStream)> ownedStream(stream, releaseStream);
    HFMultipleFaceData faces{};
    ret = HFExecuteFaceTrack(m_session, stream, &faces);
    if (ret == HSUCCEED) {
        for (int i = 0; i < faces.detectedNum && i < 8; ++i) {
            HPoint2f points[5]{};
            if (HFGetFaceFiveKeyPointsFromFaceToken(faces.tokens[i], points, 5) != HSUCCEED) continue;
            auto left = points[0], right = points[1];
            if (left.x > right.x) std::swap(left, right);
            const float dx = right.x-left.x, dy = right.y-left.y;
            const float distance = std::hypot(dx,dy);
            if (distance < 2) continue;
            poses.push_back({(left.x+right.x)*0.5f + dy*0.95f,
                             (left.y+right.y)*0.5f - dx*0.95f,
                             distance*2.8f, std::atan2(dy,dx)});
        }
    } else m_error = "人脸检测失败: " + std::to_string(ret);
    return ret == HSUCCEED;
}
}
