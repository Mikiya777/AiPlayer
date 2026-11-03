#include "diagnostics/Performance.h"
#include "writer/VideoEncoder.h"
#include <stdexcept>
#include <cstdlib>
#ifdef AIPLAYER_HAVE_LIBYUV
#include <libyuv/convert.h>
#endif
namespace PLAYER {
VideoEncoder::~VideoEncoder(){sws_freeContext(m_scale);avcodec_free_context(&m_context);}
void VideoEncoder::Open(Muxer& muxer,int width,int height,const std::string& name)
{
    if(m_context)throw std::logic_error("视频编码器只能打开一次");
    const char* converter=std::getenv("AIPLAYER_RECORD_CONVERTER");
    m_use_libyuv=converter && std::string(converter)=="libyuv";
#ifndef AIPLAYER_HAVE_LIBYUV
    if(m_use_libyuv)throw std::runtime_error("此构建未启用 libyuv 对照测试");
#endif
    if(m_use_libyuv && (width%2 || height%2))
        throw std::runtime_error("libyuv 对照路径暂仅支持偶数宽高");
    const auto* codec=avcodec_find_encoder_by_name(name.c_str());
    if(!codec)throw std::runtime_error("视频编码器不可用: "+name);
    m_context=avcodec_alloc_context3(codec);if(!m_context)throw std::bad_alloc();
    m_context->width=width;m_context->height=height;m_context->pix_fmt=AV_PIX_FMT_YUV420P;
    m_context->time_base={1,90000};m_context->framerate={30,1};m_context->gop_size=60;
    m_context->max_b_frames=0;m_context->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* options=nullptr;
    // 实时录制不做多帧前瞻，避免编码器内部积压整组 4K 图像。
    if(name=="libx264"){av_dict_set(&options,"preset","veryfast",0);av_dict_set(&options,"crf","20",0);av_dict_set(&options,"tune","zerolatency",0);}
    int ret=avcodec_open2(m_context,codec,&options);av_dict_free(&options);CheckWrite(ret,"打开视频编码器");
    m_stream=muxer.AddStream(*m_context);m_muxer=&muxer;
}
void VideoEncoder::Send(const AVFrame* rgba,int64_t pts,int64_t duration)
{
    AVFrame* frame=nullptr;
    if(rgba){
        PerformanceScope measure("record_rgba_yuv_convert");
        {
        PerformanceScope preparation("record_frame_prepare");
        if(!m_frame) {
            PerformanceScope allocation("record_frame_allocate");
            FramePtr allocated(av_frame_alloc());if(!allocated)throw std::bad_alloc();
            allocated->format=m_context->pix_fmt;
            allocated->width=m_context->width;allocated->height=m_context->height;
            CheckWrite(av_frame_get_buffer(allocated.get(),32),"分配编码图像");
            m_frame=std::move(allocated);
        }
        frame=m_frame.get();
        // 编码器可能仍持有上一帧：确保可写，不能直接覆盖共享缓冲。
        Performance::Get().Add("record_frame_copy_on_write_count",
                               av_frame_is_writable(frame)?0:1);
        CheckWrite(av_frame_make_writable(frame),"准备可写编码图像");
        frame->pts=pts;frame->pkt_duration=duration;
        }
        if(m_use_libyuv) {
#ifdef AIPLAYER_HAVE_LIBYUV
            // 本实验仅替换同尺寸 RGBA -> BT.601 limited I420；不负责缩放。
            if(rgba->format!=AV_PIX_FMT_RGBA || rgba->width!=frame->width || rgba->height!=frame->height)
                throw std::runtime_error("libyuv 对照路径要求同尺寸 RGBA 输入");
            PerformanceScope conversion("record_libyuv_convert");
            // libyuv 的 ABGR 命名指小端寄存器顺序，实际内存字节是 R,G,B,A。
            int converted=libyuv::ABGRToI420(rgba->data[0],rgba->linesize[0],
                frame->data[0],frame->linesize[0],frame->data[1],frame->linesize[1],
                frame->data[2],frame->linesize[2],frame->width,frame->height);
            if(converted!=0)throw std::runtime_error("libyuv 转换失败");
#endif
        } else {
        {
        PerformanceScope context("record_scale_context");
        m_scale=sws_getCachedContext(m_scale,rgba->width,rgba->height,AV_PIX_FMT_RGBA,
            frame->width,frame->height,AV_PIX_FMT_YUV420P,SWS_BILINEAR,nullptr,nullptr,nullptr);
        if(!m_scale)throw std::bad_alloc();
        }
        {
        // 单独测转换调用；新内存首次写入的成本也可能体现在这里。
        PerformanceScope conversion("record_sws_scale");
        CheckWrite(sws_scale(m_scale,rgba->data,rgba->linesize,0,rgba->height,frame->data,frame->linesize),"转换编码图像");
        }
        }
    }
    { PerformanceScope measure("record_encode_send");
      CheckWrite(avcodec_send_frame(m_context,frame),"发送视频帧"); }
    PacketPtr packet(av_packet_alloc());if(!packet)throw std::bad_alloc();
    int ret;
    auto Receive=[&]{PerformanceScope measure("record_encode_receive");return avcodec_receive_packet(m_context,packet.get());};
    while((ret=Receive())==0){
        if(packet->duration<=0)packet->duration=duration;
        m_muxer->Write(*packet,m_context->time_base,m_stream);
    }
    if(ret!=AVERROR_EOF && ret!=AVERROR(EAGAIN))CheckWrite(ret,"接收视频编码包");
}
}
