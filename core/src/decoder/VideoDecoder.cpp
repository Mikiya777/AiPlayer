#include "diagnostics/Performance.h"
#include "decoder/VideoDecoder.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
extern "C" {
#include <libavutil/hwcontext.h>
}

namespace PLAYER {
VideoDecoder::~VideoDecoder() { Close(); }

AVPixelFormat VideoDecoder::SelectHardwareFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    for(auto p=formats; *p!=AV_PIX_FMT_NONE; ++p)
        if(*p==AV_PIX_FMT_VIDEOTOOLBOX)return *p;
    // 对照实验不静默回退，否则测到的可能仍是软件解码。
    return AV_PIX_FMT_NONE;
}

int VideoDecoder::Open(const AVStream& stream)
{
    Close();
    if (!stream.codecpar || stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return AVERROR(EINVAL);
    const char* mode=std::getenv("AIPLAYER_VIDEO_DECODER");
    const bool hardware=mode && std::strcmp(mode,"videotoolbox")==0;
    if(mode && !hardware && std::strcmp(mode,"software")!=0)return AVERROR(EINVAL);

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

    if(hardware) {
        bool supported=false;
        for(int i=0; const auto* config=avcodec_get_hw_config(codec,i); ++i) {
            if(config->device_type==AV_HWDEVICE_TYPE_VIDEOTOOLBOX &&
               config->pix_fmt==AV_PIX_FMT_VIDEOTOOLBOX &&
               (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                supported=true;break;
            }
        }
        if(!supported){Close();return AVERROR(ENOSYS);}
        ret=av_hwdevice_ctx_create(&m_context->hw_device_ctx,
                                  AV_HWDEVICE_TYPE_VIDEOTOOLBOX,nullptr,nullptr,0);
        if(ret<0){Close();return ret;}
        m_context->get_format=&VideoDecoder::SelectHardwareFormat;
        m_hardware_frame.reset(av_frame_alloc());
        if(!m_hardware_frame){Close();return AVERROR(ENOMEM);}
    }

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
    int ret;
    {
        PerformanceScope measure("video_decode_receive");
        ret=avcodec_receive_frame(m_context, m_hardware_frame?m_hardware_frame.get():frame);
    }
    if(ret<0)return ret;
    if(m_hardware_frame) {
        av_frame_unref(frame);
        if(m_hardware_frame->format!=AV_PIX_FMT_VIDEOTOOLBOX) {
            av_frame_unref(m_hardware_frame.get());
            return AVERROR(ENOTSUP);
        }
        {
            // 第一阶段保留 CPU 渲染输入，下载成本必须计入硬解方案。
            PerformanceScope transfer("video_hw_download");
            ret=av_hwframe_transfer_data(frame,m_hardware_frame.get(),0);
        }
        if(ret>=0)ret=av_frame_copy_props(frame,m_hardware_frame.get());
        av_frame_unref(m_hardware_frame.get());
        if(ret<0){av_frame_unref(frame);return ret;}
        Performance::Get().Add("video_hw_frames_count",1);
    }
    Performance::Get().Add("decoded_frames_count",1);
    return 0;
}

void VideoDecoder::Flush()
{
    if (m_context) avcodec_flush_buffers(m_context);
    if (m_hardware_frame) av_frame_unref(m_hardware_frame.get());
}

void VideoDecoder::Close()
{
    avcodec_free_context(&m_context);
    m_hardware_frame.reset();
    m_time_base = {0, 1};
}
} // namespace PLAYER
