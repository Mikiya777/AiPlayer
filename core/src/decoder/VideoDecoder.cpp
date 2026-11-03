#include "diagnostics/Performance.h"
#include "decoder/VideoDecoder.h"
#include <cerrno>

namespace PLAYER {
VideoDecoder::~VideoDecoder() { Close(); }

int VideoDecoder::Open(const AVStream& stream)
{
    Close();
    if (!stream.codecpar || stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return AVERROR(EINVAL);

    // 1. AVCodec 描述解码器实现，由 FFmpeg 持有，不需要我们释放。
    const AVCodec* codec = avcodec_find_decoder(stream.codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;

    // 2. 为这一路解码任务分配独立的配置和运行状态。
    m_context = avcodec_alloc_context3(codec);
    if (!m_context) return AVERROR(ENOMEM);

    // 3. 从流描述复制编码参数，而不是把媒体数据复制进上下文。
    int ret = avcodec_parameters_to_context(m_context, stream.codecpar);
    if (ret < 0) {
        Close();
        return ret;
    }
    m_context->pkt_timebase = stream.time_base;

    // 4. 初始化解码器；真正的数据由之后的 SendPacket 提供。
    ret = avcodec_open2(m_context, codec, nullptr);
    if (ret < 0) {
        Close();
        return ret;
    }
    m_time_base = stream.time_base;
    return 0;
}

int VideoDecoder::SendPacket(const AVPacket* packet)
{
    if (!m_context) return AVERROR(EINVAL);
    PerformanceScope measure("video_decode_send");
    return avcodec_send_packet(m_context, packet);
}

int VideoDecoder::ReceiveFrame(AVFrame* frame)
{
    if (!m_context || !frame) return AVERROR(EINVAL);
    PerformanceScope measure("video_decode_receive");
    const int ret=avcodec_receive_frame(m_context, frame);
    if(ret==0)Performance::Get().Add("decoded_frames_count",1);
    return ret;
}

void VideoDecoder::Flush()
{
    if (m_context) avcodec_flush_buffers(m_context);
}

void VideoDecoder::Close()
{
    avcodec_free_context(&m_context);
    m_time_base = {0, 1};
}
} // namespace PLAYER
