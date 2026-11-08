#include "diagnostics/Performance.h"
#include "writer/VideoEncoder.h"
#include <stdexcept>
#include <cstdlib>
#include <memory>
#ifdef AIPLAYER_HAVE_LIBYUV
#include <libyuv/convert.h>
#endif
namespace PLAYER {
VideoEncoder::~VideoEncoder() {
  sws_freeContext(m_scale);
  avcodec_free_context(&m_context);
}
void VideoEncoder::Open(Muxer &muxer, int width, int height, const std::string &name) {
  if (m_context) throw std::logic_error("视频编码器只能打开一次");
  const char *converter = std::getenv("AIPLAYER_RECORD_CONVERTER");
  m_use_libyuv = converter && std::string(converter) == "libyuv";
#ifndef AIPLAYER_HAVE_LIBYUV
  if (m_use_libyuv) throw std::runtime_error("此构建未启用 libyuv 对照测试");
#endif
  if (m_use_libyuv && (width % 2 || height % 2))
    throw std::runtime_error("libyuv 对照路径暂仅支持偶数宽高");
  const bool automatic = name == "auto";
  int ret = OpenCodec(width, height, automatic ? "h264_videotoolbox" : name);
  if (ret < 0 && automatic && ret != AVERROR(ENOMEM)) {
    char reason[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(ret, reason, sizeof(reason));
    av_log(nullptr, AV_LOG_WARNING,
           "AiPlayer: VideoToolbox encoder unavailable (%s); trying libx264.\n", reason);
    ret = OpenCodec(width, height, "libx264");
  }
  CheckWrite(ret, "打开视频编码器");
  av_log(m_context, AV_LOG_INFO, "AiPlayer: recording encoder=%s\n", m_context->codec->name);
  // 只有选定的编码器初始化成功后才创建流，避免回退时留下多余流或错误时间基。
  m_stream = muxer.AddStream(*m_context);
  m_muxer = &muxer;
}
int VideoEncoder::OpenCodec(int width, int height, const std::string &name) {
  const auto *codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) return AVERROR_ENCODER_NOT_FOUND;
  const auto release = [](AVCodecContext *context) { avcodec_free_context(&context); };
  std::unique_ptr<AVCodecContext, decltype(release)> context(avcodec_alloc_context3(codec), release);
  if (!context) return AVERROR(ENOMEM);
  const bool hardware = name == "h264_videotoolbox";
  context->width = width;
  context->height = height;
  context->pix_fmt = AV_PIX_FMT_YUV420P;
  context->time_base = {1, 90000};
  context->framerate = {30, 1};
  context->gop_size = 60;
  if (hardware) {
    context->time_base = {1, 30};
    // 保留已有硬编码率策略；与软件 CRF 20 不构成等画质对照。
    context->bit_rate = std::max<int64_t>(2000000, int64_t(width) * height * 40000000 / (3840 * 2160));
  }
  context->max_b_frames = 0;
  context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  AVDictionary *options = nullptr;
  if (name == "libx264") {
    av_dict_set(&options, "preset", "veryfast", 0);
    av_dict_set(&options, "crf", "20", 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
  }
  if (hardware) {
    av_dict_set(&options, "realtime", "1", 0);
    av_dict_set(&options, "allow_sw", "0", 0);
    av_dict_set(&options, "profile", "high", 0);
    av_dict_set(&options, "bf", "0", 0);
  }
  const int ret = avcodec_open2(context.get(), codec, &options);
  av_dict_free(&options);
  if (ret < 0) return ret;
  m_context = context.release();
  m_videotoolbox = hardware;
  return 0;
}
void VideoEncoder::Send(const AVFrame *rgba, int64_t pts, int64_t duration) {
  // 对外继续接受 1/90000 时间基，避免改变 FileWriter 的时间轴。
  const int64_t encoderPts = av_rescale_q(pts, AVRational{1, 90000}, m_context->time_base);
  const int64_t encoderDuration = av_rescale_q(duration, AVRational{1, 90000}, m_context->time_base);
  if (m_videotoolbox && rgba && (pts < 0 || duration != 3000 || pts % 3000 || (m_last_input_pts != AV_NOPTS_VALUE && encoderPts != m_last_input_pts + 1)))
    throw std::runtime_error("VideoToolbox 对照要求连续 30fps 时间戳");
  AVFrame *frame = nullptr;
  if (rgba) {
    PerformanceScope measure("record_rgba_yuv_convert");
    {
      PerformanceScope preparation("record_frame_prepare");
      if (!m_frame) {
        PerformanceScope allocation("record_frame_allocate");
        FramePtr allocated(av_frame_alloc());
        if (!allocated) throw std::bad_alloc();
        allocated->format = m_context->pix_fmt;
        allocated->width = m_context->width;
        allocated->height = m_context->height;
        CheckWrite(av_frame_get_buffer(allocated.get(), 32), "分配编码图像");
        m_frame = std::move(allocated);
      }
      frame = m_frame.get();
      // 编码器可能仍持有上一帧：确保可写，不能直接覆盖共享缓冲。
      Performance::Get().Add("record_frame_copy_on_write_count",
                             av_frame_is_writable(frame) ? 0 : 1);
      CheckWrite(av_frame_make_writable(frame), "准备可写编码图像");
      if (Performance::Get().Active()) {
        size_t bytes = 0;
        for (const auto *buffer : frame->buf)
          if (buffer) bytes += buffer->size;
        Performance::Get().Add("memory_encoder_yuv_buffer_mib", bytes / 1048576.0);
      }
      frame->pts = encoderPts;
      frame->pkt_duration = encoderDuration;
    }
    if (m_use_libyuv) {
#ifdef AIPLAYER_HAVE_LIBYUV
      // 本实验仅替换同尺寸 RGBA -> BT.601 limited I420；不负责缩放。
      if (rgba->format != AV_PIX_FMT_RGBA || rgba->width != frame->width || rgba->height != frame->height)
        throw std::runtime_error("libyuv 对照路径要求同尺寸 RGBA 输入");
      PerformanceScope conversion("record_libyuv_convert");
      // libyuv 的 ABGR 命名指小端寄存器顺序，实际内存字节是 R,G,B,A。
      int converted = libyuv::ABGRToI420(rgba->data[0], rgba->linesize[0],
                                         frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                         frame->data[2], frame->linesize[2], frame->width, frame->height);
      if (converted != 0) throw std::runtime_error("libyuv 转换失败");
#endif
    } else {
      {
        PerformanceScope context("record_scale_context");
        m_scale = sws_getCachedContext(m_scale, rgba->width, rgba->height, AV_PIX_FMT_RGBA,
                                       frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_scale) throw std::bad_alloc();
      }
      {
        // 单独测转换调用；新内存首次写入的成本也可能体现在这里。
        PerformanceScope conversion("record_sws_scale");
        CheckWrite(sws_scale(m_scale, rgba->data, rgba->linesize, 0, rgba->height, frame->data, frame->linesize), "转换编码图像");
      }
    }
  }
  const double submitted = Performance::Get().Active() ? Performance::Now() : 0;
  {
    PerformanceScope measure("record_encode_send");
    CheckWrite(avcodec_send_frame(m_context, frame), "发送视频帧");
  }
  if (m_videotoolbox && frame) {
    m_pending.push_back({encoderPts, encoderDuration, submitted});
    m_last_input_pts = encoderPts;
  }
  PacketPtr packet(av_packet_alloc());
  if (!packet) throw std::bad_alloc();
  int ret;
  auto Receive = [&] {PerformanceScope measure("record_encode_receive");return avcodec_receive_packet(m_context,packet.get()); };
  while ((ret = Receive()) == 0) {
    if (m_videotoolbox) {
      if (m_pending.empty() || packet->pts != m_pending.front().pts ||
          m_context->max_b_frames != 0 || m_context->has_b_frames != 0)
        throw std::runtime_error("VideoToolbox 时间戳不符: pts=" + std::to_string(packet->pts) + ", dts=" + std::to_string(packet->dts) + ", expected=" + std::to_string(m_pending.empty() ? -1 : m_pending.front().pts) + ", max_b=" + std::to_string(m_context->max_b_frames) + ", has_b=" + std::to_string(m_context->has_b_frames));
      const auto pending = m_pending.front();
      m_pending.pop_front();
      // 仅限已禁用 B 帧且输出 PTS 与输入顺序一致的路径；不修补任意编码器。
      if (packet->dts != packet->pts) throw std::runtime_error("VideoToolbox 无重排输出 DTS 与 PTS 不一致");
      packet->duration = pending.duration;
      Performance::Get().Add("record_vt_packets_count", 1);
      if (pending.submitted) Performance::Get().Add("record_encode_output_latency", Performance::Now() - pending.submitted);
    } else if (packet->duration <= 0)
      packet->duration = encoderDuration;
    m_muxer->Write(*packet, m_context->time_base, m_stream);
  }
  if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) CheckWrite(ret, "接收视频编码包");
  if (m_videotoolbox && !rgba && (ret != AVERROR_EOF || !m_pending.empty()))
    throw std::runtime_error("VideoToolbox 排空不完整");
}
} // namespace PLAYER
