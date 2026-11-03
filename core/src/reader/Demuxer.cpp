#include "diagnostics/Performance.h"
#include "reader/Demuxer.h"
#include <cerrno>
#include <cmath>
#include <limits>

namespace PLAYER {
Demuxer::~Demuxer() { Close(); }

int Demuxer::Open(const std::string& url)
{
    Close();
    if (url.empty()) return AVERROR(EINVAL);

    // &m_context 允许 FFmpeg 创建上下文，并把地址写回成员变量。
    int ret = avformat_open_input(&m_context, url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        Close();
        return ret;
    }
    ret = avformat_find_stream_info(m_context, nullptr);
    if (ret < 0) {
        Close();
        return ret;
    }
    return 0;
}

void Demuxer::Close()
{
    // 同时关闭输入并释放关联流信息；函数会将 m_context 置空。
    avformat_close_input(&m_context);
}

int Demuxer::GetBestStream(AVMediaType type) const
{
    if (!m_context) return AVERROR(EINVAL);
    return av_find_best_stream(m_context, type, -1, -1, nullptr, 0);
}

unsigned int Demuxer::GetStreamCount() const
{
    return m_context ? m_context->nb_streams : 0;
}

double Demuxer::GetDurationSeconds() const
{
    if (!m_context || m_context->duration == AV_NOPTS_VALUE) return 0.0;
    return static_cast<double>(m_context->duration) / AV_TIME_BASE;
}

const AVStream* Demuxer::GetStream(int index) const
{
    if (!m_context || index < 0 || static_cast<unsigned int>(index) >= m_context->nb_streams)
        return nullptr;
    return m_context->streams[index];
}

int Demuxer::ReadPacket(AVPacket* packet)
{
    PerformanceScope measure("demux_read");
    if (!packet) return AVERROR(EINVAL);
    av_packet_unref(packet);
    if (!m_context) return AVERROR(EINVAL);
    return av_read_frame(m_context, packet);
}

int Demuxer::Seek(double seconds)
{
    if (!m_context || !std::isfinite(seconds) || seconds < 0.0) return AVERROR(EINVAL);
    const double timestamp = seconds * AV_TIME_BASE;
    // double 表示 INT64_MAX 时会向上舍入到 2^63，等于该边界也不能转换。
    if (timestamp >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return AVERROR(EINVAL);
    const int64_t target = static_cast<int64_t>(timestamp);
    return avformat_seek_file(m_context, -1, std::numeric_limits<int64_t>::min(), target,
                              std::numeric_limits<int64_t>::max(), AVSEEK_FLAG_BACKWARD);
}
} // namespace PLAYER
