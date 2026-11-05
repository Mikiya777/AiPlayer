#include "effects/FaceStickerFilter.h"
#include "diagnostics/Performance.h"
#ifdef AIPLAYER_HAVE_LIBYUV
#include <libyuv/scale_argb.h>
#endif
#include <algorithm>
#include <cstdlib>
#include <limits>
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#include <cmath>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace PLAYER {
FaceStickerFilter::~FaceStickerFilter() {
  if (m_session) HFReleaseInspireFaceSession(m_session);
  sws_freeContext(m_scaler);
}
bool FaceStickerFilter::Initialize(const std::string &modelPath) {
  if (m_session) return true;
  // SDK 全局模型由进程持有，session 只由检测工作线程使用。
  static std::mutex sc_launch_mutex;
  static bool sc_launched = false;
  std::lock_guard<std::mutex> lock(sc_launch_mutex);
  if (!sc_launched) {
    const auto ret = HFLaunchInspireFace(modelPath.c_str());
    if (ret != HSUCCEED) {
      m_error = "人脸模型加载失败: " + std::to_string(ret);
      return false;
    }
    sc_launched = true;
  }
  // 每次检测独立输入，适应抽帧、跳转和场景切换，避免沿用旧跟踪状态。
  const auto ret = HFCreateInspireFaceSessionOptional(0, HF_DETECT_MODE_ALWAYS_DETECT, 8, 320, -1, &m_session);
  if (ret != HSUCCEED) {
    m_error = "创建人脸检测器失败: " + std::to_string(ret);
    return false;
  }
  return true;
}
bool FaceStickerFilter::Detect(const AVFrame &frame, std::vector<FaceStickerPose> &poses) {
  poses.clear();
  if (!m_session || frame.format != AV_PIX_FMT_RGBA || frame.width <= 0 || frame.height <= 0 || !frame.data[0]) return false;
  if (frame.width > std::numeric_limits<int>::max() / 4 ||
      av_image_check_size(frame.width, frame.height, 0, nullptr) < 0) return false;
  const size_t sourceStride = static_cast<size_t>(frame.width) * 4;
  if (std::abs(static_cast<int64_t>(frame.linesize[0])) < static_cast<int64_t>(sourceStride)) return false;
  // 启用 libyuv 的构建默认使用小图检测；full 保留原输入，960 为 swscale 对照。
  const char *mode = std::getenv("AIPLAYER_FACE_INPUT");
#ifdef AIPLAYER_HAVE_LIBYUV
  if (!mode) mode = "libyuv-960";
#endif
  const bool useLibyuv = mode && std::strcmp(mode, "libyuv-960") == 0;
#ifndef AIPLAYER_HAVE_LIBYUV
  if (useLibyuv) {
    m_error = "此构建未启用 libyuv 检测缩放";
    return false;
  }
#endif
  const bool fullSize = !useLibyuv && (!mode || std::strcmp(mode, "960") != 0);
  const double ratio = fullSize ? 1.0 : std::min(1.0, 960.0 / std::max(frame.width, frame.height));
  const int width = std::max(1, static_cast<int>(std::lround(frame.width * ratio)));
  const int height = std::max(1, static_cast<int>(std::lround(frame.height * ratio)));
  const int stride = width * 4;
  {
    PerformanceScope measure("face_input_prepare");
    m_pixels.resize(static_cast<size_t>(stride) * height);
    if (width == frame.width && height == frame.height) {
      for (int y = 0; y < height; ++y)
        std::memcpy(m_pixels.data() + static_cast<size_t>(y) * stride,
                    frame.data[0] + static_cast<ptrdiff_t>(y) * frame.linesize[0], stride);
    } else if (useLibyuv) {
#ifdef AIPLAYER_HAVE_LIBYUV
      // 四通道分别缩放，不转换 RGBA 的通道排列。
      if (libyuv::ARGBScale(frame.data[0], frame.linesize[0], frame.width, frame.height,
                            m_pixels.data(), stride, width, height, libyuv::kFilterBilinear) != 0) {
        m_error = "libyuv 检测输入缩放失败";
        return false;
      }
#endif
    } else {
      m_scaler = sws_getCachedContext(m_scaler, frame.width, frame.height, AV_PIX_FMT_RGBA,
                                      width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                      nullptr, nullptr, nullptr);
      if (!m_scaler) {
        m_error = "无法创建检测输入缩放器";
        return false;
      }
      uint8_t *output[] = {m_pixels.data(), nullptr, nullptr, nullptr};
      const int lines[] = {stride, 0, 0, 0};
      if (sws_scale(m_scaler, frame.data, frame.linesize, 0, frame.height, output, lines) != height) {
        m_error = "检测输入缩放失败";
        return false;
      }
    }
  }
  Performance::Get().Add("memory_face_input_capacity_mib", m_pixels.capacity() / 1048576.0);
  Performance::Get().Add("face_input_mib", m_pixels.size() / 1048576.0);
  HFImageData image{m_pixels.data(), width, height, HF_STREAM_RGBA, HF_CAMERA_ROTATION_0};
  HFImageStream stream = nullptr;
  auto ret = HFCreateImageStream(&image, &stream);
  if (ret != HSUCCEED) {
    m_error = "创建检测图像失败: " + std::to_string(ret);
    return false;
  }
  // poses 扩容等操作可能抛异常，图像句柄仍必须归还 SDK。
  const auto releaseStream = [](auto *value) { HFReleaseImageStream(value); };
  std::unique_ptr<std::remove_pointer_t<HFImageStream>, decltype(releaseStream)> ownedStream(stream, releaseStream);
  HFMultipleFaceData faces{};
  {
    PerformanceScope measure("face_sdk_detect");
    ret = HFExecuteFaceTrack(m_session, stream, &faces);
  }
  if (ret == HSUCCEED) Performance::Get().Add("face_detected_count", faces.detectedNum);
  if (ret == HSUCCEED) {
    for (int i = 0; i < faces.detectedNum && i < 8; ++i) {
      HPoint2f points[5]{};
      if (HFGetFaceFiveKeyPointsFromFaceToken(faces.tokens[i], points, 5) != HSUCCEED) continue;
      auto left = points[0], right = points[1];
      // 按实际整数尺寸分别映射 x/y，避免尺寸取整导致位置和角度误差。
      const float scaleX = static_cast<float>(frame.width) / width;
      const float scaleY = static_cast<float>(frame.height) / height;
      left.x *= scaleX;
      right.x *= scaleX;
      left.y *= scaleY;
      right.y *= scaleY;
      if (left.x > right.x) std::swap(left, right);
      const float dx = right.x - left.x, dy = right.y - left.y;
      const float distance = std::hypot(dx, dy);
      if (distance < 2) continue;
      poses.push_back({(left.x + right.x) * 0.5f + dy * 0.95f,
                       (left.y + right.y) * 0.5f - dx * 0.95f,
                       distance * 2.8f, std::atan2(dy, dx)});
    }
  } else
    m_error = "人脸检测失败: " + std::to_string(ret);
  return ret == HSUCCEED;
}
} // namespace PLAYER
