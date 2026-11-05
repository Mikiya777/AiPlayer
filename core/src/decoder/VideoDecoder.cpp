#include "diagnostics/Performance.h"
#include "decoder/VideoDecoder.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace PLAYER {
VideoDecoder::~VideoDecoder() {
  Close();
}

AVPixelFormat VideoDecoder::SelectHardwareFormat(AVCodecContext *context, const AVPixelFormat *formats) {
  for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
    if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
  const auto *decoder = static_cast<const VideoDecoder *>(context->opaque);
  if (decoder && decoder->m_allow_software_fallback) {
    // 部分流直到首包才协商像素格式；硬件初始化失败后，FFmpeg 会再次调用这里。
    for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) {
      const auto *description = av_pix_fmt_desc_get(*p);
      if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        av_log(context, AV_LOG_WARNING,
               "AiPlayer: VideoToolbox format unavailable; falling back to software decoding.\n");
        return *p;
      }
    }
  }
  // 强制硬解模式保留失败语义，避免性能对照悄悄使用软件路径。
  return AV_PIX_FMT_NONE;
}

int VideoDecoder::Open(const AVStream &stream) {
  Close();
  if (!stream.codecpar || stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
    return AVERROR(EINVAL);
  const char *mode = std::getenv("AIPLAYER_VIDEO_DECODER");
  // 未指定时保持原软件默认值；产品启动脚本显式选择 auto。
  const bool automatic = mode && std::strcmp(mode, "auto") == 0;
  const bool hardware = automatic || (mode && std::strcmp(mode, "videotoolbox") == 0);
  if (mode && !hardware && std::strcmp(mode, "software") != 0) return AVERROR(EINVAL);

  // AVCodec 描述由 FFmpeg 持有，每次尝试都使用新上下文与原始流参数。
  const AVCodec *codec = avcodec_find_decoder(stream.codecpar->codec_id);
  if (!codec) return AVERROR_DECODER_NOT_FOUND;
  const auto initialize = [&](bool useHardware) -> int {
    Close();
    m_allow_software_fallback = automatic;
    m_context = avcodec_alloc_context3(codec);
    if (!m_context) return AVERROR(ENOMEM);
    int ret = avcodec_parameters_to_context(m_context, stream.codecpar);
    if (ret < 0) return ret;
    m_context->pkt_timebase = stream.time_base;

    if (useHardware) {
      bool supported = false;
      for (int i = 0; const auto *config = avcodec_get_hw_config(codec, i); ++i) {
        if (config->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX &&
            config->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
          supported = true;
          break;
        }
      }
      if (!supported) return AVERROR(ENOSYS);
      ret = av_hwdevice_ctx_create(&m_context->hw_device_ctx,
                                  AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
      if (ret < 0) return ret;
      m_context->opaque = this;
      m_context->get_format = &VideoDecoder::SelectHardwareFormat;
      m_hardware_frame.reset(av_frame_alloc());
      if (!m_hardware_frame) return AVERROR(ENOMEM);
    }
    return avcodec_open2(m_context, codec, nullptr);
  };

  int ret = initialize(hardware);
  if (ret < 0 && hardware && automatic && ret != AVERROR(ENOMEM)) {
    char reason[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(ret, reason, sizeof(reason));
    av_log(nullptr, AV_LOG_WARNING,
           "AiPlayer: %s VideoToolbox open failed (%s); trying software decoding.\n", codec->name, reason);
    ret = initialize(false);
  }
  if (ret < 0) {
    Close();
    return ret;
  }
  av_log(m_context, AV_LOG_INFO, "AiPlayer: decoder=%s, mode=%s\n", codec->name, mode ? mode : "software");
  m_time_base = stream.time_base;
  return 0;
}

int VideoDecoder::SendPacket(const AVPacket *packet) {
  if (!m_context) return AVERROR(EINVAL);
  PerformanceScope measure("video_decode_send");
  return avcodec_send_packet(m_context, packet);
}

int VideoDecoder::ReceiveFrame(AVFrame *frame) {
  if (!m_context || !frame) return AVERROR(EINVAL);
  int ret;
  {
    PerformanceScope measure("video_decode_receive");
    ret = avcodec_receive_frame(m_context, m_hardware_frame ? m_hardware_frame.get() : frame);
  }
  if (ret < 0) return ret;
  if (m_hardware_frame) {
    av_frame_unref(frame);
    if (m_hardware_frame->format != AV_PIX_FMT_VIDEOTOOLBOX) {
      if (!m_allow_software_fallback) {
        av_frame_unref(m_hardware_frame.get());
        return AVERROR(ENOTSUP);
      }
      // 格式协商已回退：输出本身是 CPU 帧，不需要硬件下载。
      av_frame_move_ref(frame, m_hardware_frame.get());
      Performance::Get().Add("decoded_frames_count", 1);
      return 0;
    }
    {
      // 第一阶段保留 CPU 渲染输入，下载成本必须计入硬解方案。
      PerformanceScope transfer("video_hw_download");
      ret = av_hwframe_transfer_data(frame, m_hardware_frame.get(), 0);
    }
    if (ret >= 0) ret = av_frame_copy_props(frame, m_hardware_frame.get());
    av_frame_unref(m_hardware_frame.get());
    if (ret < 0) {
      av_frame_unref(frame);
      return ret;
    }
    Performance::Get().Add("video_hw_frames_count", 1);
  }
  Performance::Get().Add("decoded_frames_count", 1);
  return 0;
}

void VideoDecoder::Flush() {
  if (m_context) avcodec_flush_buffers(m_context);
  if (m_hardware_frame) av_frame_unref(m_hardware_frame.get());
}

void VideoDecoder::Close() {
  avcodec_free_context(&m_context);
  m_hardware_frame.reset();
  m_allow_software_fallback = false;
  m_time_base = {0, 1};
}
} // namespace PLAYER
