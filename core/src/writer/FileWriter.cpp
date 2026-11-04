#include "diagnostics/Performance.h"
#include "writer/FileWriter.h"
#include "writer/VideoEncoder.h"
#include "writer/AudioEncoder.h"
#include "reader/FileReader.h"
#include "audio/AudioPipeline.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <iostream>
namespace PLAYER {
namespace {
struct AudioComplete {};
bool ValidTime(double time)
{
    // 同时为 90kHz 视频时间戳和 48kHz 音频采样计数保留整数空间。
    return std::isfinite(time) && time>=0 &&
        time<static_cast<double>(std::numeric_limits<int64_t>::max())/90000;
}
void WriteAudio(AudioEncoder& encoder,FileReader& reader,double start,double end)
{
    AudioPipeline pipeline;bool opened=false;int64_t written=0;
    const int64_t total=std::max<int64_t>(0,std::llround((end-start)*48000));
    const int64_t origin=std::llround(start*48000);
    auto Output=[&](const FramePtr& pcm){
        if(!pcm)return;
        const int64_t relative=pcm->pts-origin;
        if(relative>=total)throw AudioComplete{};
        const int64_t begin=std::max<int64_t>(written,std::max<int64_t>(0,relative));
        const int64_t finish=std::min<int64_t>(total,relative+pcm->nb_samples);
        if(finish<=begin)return;
        if(begin>written)encoder.Silence(begin-written);
        encoder.Push(*pcm,static_cast<int>(begin-relative),static_cast<int>(finish-begin));written=finish;
    };
    CheckWrite(reader.Seek(std::max(0.0,start-1.0)),"定位录制音频");
    try {
        CheckWrite(reader.ReadAll([&](const AVFrame& frame,AVRational base){
            if(!opened){AudioOutputConfig config;config.m_sample_rate=48000;config.m_sample_format=AV_SAMPLE_FMT_FLTP;
                CheckWrite(pipeline.Open(frame,config),"准备录制音频转换");opened=true;}
            FramePtr pcm;CheckWrite(pipeline.Process(frame,base,pcm),"转换录制音频");Output(pcm);
        },{}),"读取录制音频");
        if(opened){FramePtr pcm;int ret;while((ret=pipeline.Drain(pcm))==0)Output(pcm);if(ret!=AVERROR_EOF)CheckWrite(ret,"排空录制音频");}
    }catch(const AudioComplete&){}
    if(written<total)encoder.Silence(total-written);
    encoder.Finish();
}
}
FileWriter::FileWriter(RecordingConfig config):m_config(std::move(config)),m_end(m_config.m_start),m_thread(&FileWriter::Run,this){}
FileWriter::~FileWriter(){Stop(m_config.m_start);if(m_thread.joinable())m_thread.join();}
std::string FileWriter::GetError() const {std::lock_guard<std::mutex> lock(m_error_mutex);return m_error;}
void FileWriter::Submit(FramePtr frame,double time)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_stopping || m_done || !frame)return;
    if(!ValidTime(time) || time<m_config.m_start || frame->format!=AV_PIX_FMT_RGBA ||
       frame->width!=m_config.m_width || frame->height!=m_config.m_height ||
       frame->width<=0 || frame->height<=0 || !frame->data[0] ||
       std::abs(static_cast<int64_t>(frame->linesize[0]))<static_cast<int64_t>(frame->width)*4) {
        {std::lock_guard<std::mutex> errorLock(m_error_mutex);m_error="录制帧格式、尺寸或时间戳无效";}
        m_stopping=true;m_wake.notify_one();return;
    }
    if(!m_ready.load())return; // 初始化期间不积压大帧。
    if(m_actual_start.load()<0)m_actual_start.store(time);
    // 按实际行跨度计费，不能漏掉对齐填充；先检查再累加，避免溢出。
    const uint64_t bytes=static_cast<uint64_t>(std::abs(static_cast<int64_t>(frame->linesize[0])))*frame->height;
    if(bytes>64*1024*1024-m_bytes){
        {std::lock_guard<std::mutex> errorLock(m_error_mutex);m_error="录制编码跟不上，已终止录制，请降低视频分辨率";}
        m_stopping=true;m_wake.notify_one();return;
    }
    m_queue.push_back({std::move(frame),time,Performance::Get().Active()?Performance::Now():0});m_bytes+=bytes;
    Performance::Get().Add("record_queue_frames",m_queue.size());
    Performance::Get().Add("record_queue_mib",m_bytes/1048576.0);m_end=std::max(m_end,time);m_wake.notify_one();
}
void FileWriter::Fail(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    {std::lock_guard<std::mutex> errorLock(m_error_mutex);m_error=error;}
    m_stopping=true;m_wake.notify_one();
}
void FileWriter::Stop(double time)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!m_stopping){
        if(ValidTime(time))m_end=std::max(m_end,time);
        else {std::lock_guard<std::mutex> errorLock(m_error_mutex);m_error="录制结束时间无效";}
        m_stopping=true;
    }
    m_wake.notify_one();
}
void FileWriter::Run()
{
    // 同目录临时文件，正常写完 trailer 后才替换用户选择的目标。
    const auto temporary=m_config.m_path+".recording-part";
    const double startup=Performance::Now();
    try {
        if(m_config.m_width<=0 || m_config.m_height<=0 || m_config.m_width%2 || m_config.m_height%2)
            throw std::runtime_error("录制分辨率必须为正偶数");
        if(!ValidTime(m_config.m_start))
            throw std::runtime_error("录制起点无效");
        if(std::filesystem::exists(temporary))throw std::runtime_error("录制临时文件已存在，请选择其他文件名");
        if(std::filesystem::weakly_canonical(m_config.m_path)==std::filesystem::weakly_canonical(m_config.m_source))
            throw std::runtime_error("不能覆盖正在播放的源文件");
        Muxer muxer;muxer.Open(temporary,m_config.m_container);
        VideoEncoder video;video.Open(muxer,m_config.m_width,m_config.m_height,m_config.m_video_codec);
        if(std::getenv("AIPLAYER_PROFILE"))std::cerr<<"record_encoder_ready_ms="<<Performance::Now()-startup<<std::endl;
        Demuxer probe;CheckWrite(probe.Open(m_config.m_source),"探测录制源");
        const bool hasAudio=probe.GetBestStream(AVMEDIA_TYPE_AUDIO)>=0;
        FileReader audioReader;if(hasAudio)CheckWrite(audioReader.Open(m_config.m_source,true,false),"打开录制源音频");
        AudioEncoder audio;if(hasAudio)audio.Open(muxer,m_config.m_audio_codec);
        muxer.WriteHeader();
        if(std::getenv("AIPLAYER_PROFILE"))std::cerr<<"record_init_ms="<<Performance::Now()-startup<<std::endl;
        m_ready.store(true);
        FramePtr previous;int64_t next=0;double end=m_config.m_start;double start=-1;
        for(;;){
            Entry entry;
            {
                std::unique_lock<std::mutex> lock(m_mutex);m_wake.wait(lock,[this]{return m_stopping || !m_queue.empty();});
                if(m_queue.empty()){end=m_end;break;}
                entry=std::move(m_queue.front());m_queue.pop_front();
                m_bytes-=static_cast<uint64_t>(std::abs(static_cast<int64_t>(entry.m_frame->linesize[0])))*entry.m_frame->height;
            }
            if(entry.m_profile_enqueue_ms)Performance::Get().Add("record_queue_wait",Performance::Now()-entry.m_profile_enqueue_ms);
            if(!GetError().empty())throw std::runtime_error(GetError());
            if(start<0){start=entry.m_time; if(std::getenv("AIPLAYER_PROFILE"))std::cerr<<"record_first_frame_ms="<<Performance::Now()-startup<<", media_start="<<start<<std::endl;}
            const int64_t target=std::max<int64_t>(0,std::llround((entry.m_time-start)*30));
            if(!previous){
                previous=FramePtr(av_frame_clone(entry.m_frame.get()));
                if(!previous)throw std::bad_alloc();
            }
            // 输出固定 30fps；较慢渲染用上一帧补齐，视频时长仍跟随媒体时钟。
            while(next<target){video.Send(previous.get(),next*3000);++next;}
            previous=std::move(entry.m_frame);
        }
        if(!GetError().empty())throw std::runtime_error(GetError());
        if(!previous)throw std::runtime_error("没有收到可录制的视频画面");
        const int64_t count=std::max<int64_t>(1,std::llround((end-start)*30));
        while(next<count){video.Send(previous.get(),next*3000);++next;}
        video.Send(nullptr);
        if(hasAudio)WriteAudio(audio,audioReader,start,start+next/30.0);
        muxer.Finish();
        std::filesystem::rename(temporary,m_config.m_path);
    }catch(const std::exception& e){
        if(std::getenv("AIPLAYER_PROFILE"))std::cerr<<"record_failure_ms="<<Performance::Now()-startup<<", error="<<e.what()<<std::endl;
        {std::lock_guard<std::mutex> lock(m_error_mutex);m_error=e.what();}
        // 保留临时文件便于诊断，不覆盖已有成品。
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping=true;
        m_queue.clear();m_bytes=0;
        m_done.store(true); // 发布完成前释放待编码帧，失败路径也不滞留大块内存。
    }
}
}
