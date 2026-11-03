#include "writer/VideoEncoder.h"
#include <stdexcept>
namespace PLAYER {
VideoEncoder::~VideoEncoder(){sws_freeContext(m_scale);avcodec_free_context(&m_context);}
void VideoEncoder::Open(Muxer& muxer,int width,int height,const std::string& name)
{
    if(m_context)throw std::logic_error("视频编码器只能打开一次");
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
    FramePtr frame;
    if(rgba){
        frame=FramePtr(av_frame_alloc());if(!frame)throw std::bad_alloc();
        frame->format=m_context->pix_fmt;frame->width=m_context->width;frame->height=m_context->height;
        frame->pts=pts;frame->pkt_duration=duration;
        CheckWrite(av_frame_get_buffer(frame.get(),32),"分配编码图像");
        m_scale=sws_getCachedContext(m_scale,rgba->width,rgba->height,AV_PIX_FMT_RGBA,
            frame->width,frame->height,AV_PIX_FMT_YUV420P,SWS_BILINEAR,nullptr,nullptr,nullptr);
        if(!m_scale)throw std::bad_alloc();
        CheckWrite(sws_scale(m_scale,rgba->data,rgba->linesize,0,rgba->height,frame->data,frame->linesize),"转换编码图像");
    }
    CheckWrite(avcodec_send_frame(m_context,frame.get()),"发送视频帧");
    PacketPtr packet(av_packet_alloc());if(!packet)throw std::bad_alloc();
    int ret;
    while((ret=avcodec_receive_packet(m_context,packet.get()))==0){
        if(packet->duration<=0)packet->duration=duration;
        m_muxer->Write(*packet,m_context->time_base,m_stream);
    }
    if(ret!=AVERROR_EOF && ret!=AVERROR(EAGAIN))CheckWrite(ret,"接收视频编码包");
}
}
