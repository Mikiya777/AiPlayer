#include "audio/AudioTempo.h"
#include <cmath>
#include <cstdio>
extern "C" {
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}
namespace PLAYER {
AudioTempo::~AudioTempo() {
  Close();
}
void AudioTempo::Close() {
  avfilter_graph_free(&m_graph);
  m_source = m_sink = nullptr;
  m_next_pts = 0;
  m_has_pts = false;
}

int AudioTempo::Open(const AudioOutputConfig &config, double rate) {
  Close();
  if (!std::isfinite(rate) || rate < .5 || rate > 2.0 || config.m_sample_rate <= 0 ||
      !config.m_channel_layout || !av_get_sample_fmt_name(config.m_sample_format)) return AVERROR(EINVAL);
  m_graph = avfilter_graph_alloc();
  if (!m_graph) return AVERROR(ENOMEM);
  m_graph->nb_threads = 1;
  auto Create = [&](AVFilterContext **context, const char *filter, const char *args) {
    const auto *definition = avfilter_get_by_name(filter);
    return definition ? avfilter_graph_create_filter(context, definition, filter, args, nullptr, m_graph)
                      : AVERROR_FILTER_NOT_FOUND;
  };
  char args[256];
  std::snprintf(args, sizeof(args), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%llx",
                config.m_sample_rate, config.m_sample_rate, av_get_sample_fmt_name(config.m_sample_format),
                static_cast<unsigned long long>(config.m_channel_layout));
  int ret = Create(&m_source, "abuffer", args);
  AVFilterContext *tempo = nullptr;
  AVFilterContext *outputFormat = nullptr;
  if (ret >= 0) ret = Create(&tempo, "atempo", nullptr);
  // 数字选项不依赖区域设置的小数点格式。
  if (ret >= 0) ret = av_opt_set_double(tempo, "tempo", rate, AV_OPT_SEARCH_CHILDREN);
  std::snprintf(args, sizeof(args), "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%llx",
                av_get_sample_fmt_name(config.m_sample_format), config.m_sample_rate,
                static_cast<unsigned long long>(config.m_channel_layout));
  if (ret >= 0) ret = Create(&outputFormat, "aformat", args);
  if (ret >= 0) ret = Create(&m_sink, "abuffersink", nullptr);
  if (ret >= 0) ret = avfilter_link(m_source, 0, tempo, 0);
  if (ret >= 0) ret = avfilter_link(tempo, 0, outputFormat, 0);
  if (ret >= 0) ret = avfilter_link(outputFormat, 0, m_sink, 0);
  if (ret >= 0) ret = avfilter_graph_config(m_graph, nullptr);
  if (ret < 0) Close();
  return ret;
}

int AudioTempo::Send(const AVFrame *input) {
  if (!m_source) return AVERROR(EINVAL);
  if (input && input->pts == AV_NOPTS_VALUE) return AVERROR_INVALIDDATA;
  const int ret = av_buffersrc_add_frame_flags(m_source, const_cast<AVFrame *>(input), AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret >= 0 && input && !m_has_pts) {
    m_next_pts = input->pts;
    m_has_pts = true;
  }
  return ret;
}

int AudioTempo::Receive(FramePtr &output) {
  output.reset();
  if (!m_sink) return AVERROR(EINVAL);
  FramePtr frame(av_frame_alloc());
  if (!frame) return AVERROR(ENOMEM);
  const int ret = av_buffersink_get_frame(m_sink, frame.get());
  if (ret < 0) return ret;
  frame->pts = frame->best_effort_timestamp = m_next_pts;
  frame->pkt_duration = frame->nb_samples;
  m_next_pts += frame->nb_samples;
  output = std::move(frame);
  return 0;
}
} // namespace PLAYER
