#include "writer/Muxer.h"
#include <stdexcept>
namespace PLAYER {
void CheckWrite(int result,const char* operation)
{
    if(result>=0)return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(result,message,sizeof(message));
    throw std::runtime_error(std::string(operation)+": "+message);
}
Muxer::~Muxer()
{
    if(m_context){if(m_context->pb)avio_closep(&m_context->pb);avformat_free_context(m_context);}
}
void Muxer::Open(const std::string& path,const std::string& format)
{
    if(m_context)throw std::logic_error("封装器只能打开一次，请创建新的录制会话");
    CheckWrite(avformat_alloc_output_context2(&m_context,nullptr,format.c_str(),path.c_str()),"创建封装器");
    if(!m_context)throw std::runtime_error("输出容器不可用");
    // 音频在收尾阶段补齐，限制交错缓存，不能把整段视频积存在内存。
    m_context->max_interleave_delta=AV_TIME_BASE;
    CheckWrite(avio_open(&m_context->pb,path.c_str(),AVIO_FLAG_WRITE),"打开录制文件");
}
int Muxer::AddStream(const AVCodecContext& codec)
{
    if(avformat_query_codec(m_context->oformat,codec.codec_id,FF_COMPLIANCE_NORMAL)==0)
        throw std::runtime_error("编码器与容器不兼容");
    auto* stream=avformat_new_stream(m_context,nullptr);
    if(!stream)throw std::bad_alloc();
    CheckWrite(avcodec_parameters_from_context(stream->codecpar,&codec),"复制编码参数");
    stream->time_base=codec.time_base;return stream->index;
}
void Muxer::WriteHeader(){CheckWrite(avformat_write_header(m_context,nullptr),"写入文件头");}
void Muxer::Write(AVPacket& packet,AVRational base,int stream)
{
    av_packet_rescale_ts(&packet,base,m_context->streams[stream]->time_base);
    packet.stream_index=stream;
    CheckWrite(av_interleaved_write_frame(m_context,&packet),"写入音视频包");
}
void Muxer::Finish()
{
    CheckWrite(av_write_trailer(m_context),"完成文件索引");
    CheckWrite(avio_closep(&m_context->pb),"关闭录制文件");
}
}
