#include "audio/AudioPipeline.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <vector>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace PLAYER {
namespace {
// 旧版 FFmpeg 的声道布局有时为 0。只为明确的单/双声道推导默认布局；
// 多声道仅凭数量不能唯一确定各声道含义，缺失时应由上层提供布局。
int GetInputFormat(const AVFrame &frame, std::uint64_t &layout) {
  if (frame.sample_rate <= 0 || frame.channels <= 0 || frame.nb_samples <= 0 ||
      av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame.format)) <= 0)
    return AVERROR(EINVAL);
  layout = frame.channel_layout;
  if (layout == 0 && frame.channels <= 2)
    layout = av_get_default_channel_layout(frame.channels);
  if (layout == 0 || av_get_channel_layout_nb_channels(layout) != frame.channels)
    return AVERROR(EINVAL);
  return 0;
}
} // namespace

AudioPipeline::~AudioPipeline() {
  Close();
}

int AudioPipeline::Open(const AVFrame &firstFrame, const AudioOutputConfig &config) {
  Close();
  std::uint64_t layout = 0;
  int ret = GetInputFormat(firstFrame, layout);
  if (ret < 0) return ret;
  if (config.m_sample_rate <= 0 || config.m_channel_layout == 0 ||
      av_get_channel_layout_nb_channels(config.m_channel_layout) <= 0 ||
      av_get_bytes_per_sample(config.m_sample_format) <= 0)
    return AVERROR(EINVAL);

  // 1. 创建并配置：先填输出布局/格式/采样率，再填输入的三项参数。
  m_context = swr_alloc_set_opts(nullptr,
                                 config.m_channel_layout, config.m_sample_format, config.m_sample_rate,
                                 layout, static_cast<AVSampleFormat>(firstFrame.format), firstFrame.sample_rate,
                                 0, nullptr);
  if (!m_context) return AVERROR(ENOMEM);

  // 2. 初始化内部滤波器等资源。相同配置下只初始化一次，不能逐帧重新创建。
  ret = swr_init(m_context);
  if (ret < 0) {
    Close();
    return ret;
  }
  m_output_config = config;
  m_input_rate = firstFrame.sample_rate;
  m_input_channels = firstFrame.channels;
  m_input_layout = layout;
  m_input_format = static_cast<AVSampleFormat>(firstFrame.format);
  return 0;
}

int AudioPipeline::Convert(const uint8_t **input, int samples, FramePtr &output) {
  //预测下一次调用 swr_convert 时，在输入特定数量的样本后，输出缓冲区所需容纳的最大样本数（上限估计）它考虑输入采样数、采样率转换以及内部缓存，告诉我们这次需要准备多少输出空间。
  //根据“本次输入 + 已缓存样本”估算输出上界，不能直接使用输入 nb_samples。
  int capacity = swr_get_out_samples(m_context, samples);
  if (capacity < 0) return capacity;
  capacity = std::max(capacity, 1);

  FramePtr converted(av_frame_alloc());
  if (!converted) return AVERROR(ENOMEM);
  converted->format = m_output_config.m_sample_format;
  converted->sample_rate = m_output_config.m_sample_rate;
  converted->channel_layout = m_output_config.m_channel_layout;
  converted->channels = av_get_channel_layout_nb_channels(converted->channel_layout);
  converted->nb_samples = capacity; //为每个声道准备能容纳 capacity 个采样的内存
  // AVFrame 自己持有输出缓冲区引用，由 FramePtr 配套释放，无裸指针悬挂。
  int ret = av_frame_get_buffer(converted.get(), 0);
  if (ret < 0) return ret;

  //转化帧：基于配置好的上下文（SwrContext），对输入的音频数据进行“采样率转换、声道布局转换、采样格式（位深与排列方式）转换”的综合流水线处理
  /*
  swr_convert(
    m_context,                 // 已保存输入、输出两套格式
    converted->extended_data,  // 输出往哪里写
    capacity,                  // 每个输出声道最多容纳多少采样
    input,                     // 从哪些输入地址读取
    samples);                  // 每个输入声道有多少采样
  */
  const int count = swr_convert(m_context, converted->extended_data, capacity, input, samples);
  if (count < 0) {
    m_failed = true; // 内部转换错误后重新 Open，不能假设旧状态仍可重试输入。
    return count;
  }
  if (count == 0) return 0;
  if (m_next_output_pts > std::numeric_limits<std::int64_t>::max() - count) {
    m_failed = true;
    return AVERROR(ERANGE);
  }
  converted->nb_samples = count; // 有效样本数；缓冲区容量可能更大。
  converted->pts = m_next_output_pts; //从多少秒开始
  converted->best_effort_timestamp = m_next_output_pts; //与时间戳pts相同
  converted->pkt_duration = count; //持续多少秒
  // 用“首个媒体 PTS + 已输出采样数”维持连续时间轴。
  // 某帧暂存在 swr 内不会推动输出 PTS，也不会把每次取整的误差累积下去。
  m_next_output_pts += count; //下一帧的起点：加count数量计算后得出的秒数。
  output = std::move(converted);
  return 0;
}

int AudioPipeline::Process(const AVFrame &input, AVRational inputTimeBase, FramePtr &output) {
  m_error.clear();
  // 1.清理上次输出，检查当前是否允许处理
  if (output.get() == &input) return AVERROR(EINVAL); // 不支持原地覆盖输入帧。
  output.reset();
  if (!m_context || m_failed) return AVERROR(EINVAL);
  if (m_draining) return AVERROR_EOF;
  if (inputTimeBase.num <= 0 || inputTimeBase.den <= 0) return AVERROR(EINVAL);
  std::uint64_t layout = 0;

  // 2.检查输入格式是否与初始化时一致
  int ret = GetInputFormat(input, layout);
  if (ret < 0) return ret;
  if (input.sample_rate != m_input_rate || input.channels != m_input_channels ||
      layout != m_input_layout || input.format != m_input_format)
    return AVERROR_INPUT_CHANGED;

  // 3.取得时间戳，并换算成“输入采样单位”
  //优先使用pts，没有再尝试best_effort_timestamp
  const auto pts = input.pts != AV_NOPTS_VALUE ? input.pts : input.best_effort_timestamp;
  if (pts == AV_NOPTS_VALUE) {
    m_error = "音频帧缺少 PTS 和 best_effort_timestamp";
    return AVERROR_INVALIDDATA;
  }
  const AVRational inputSampleBase{1, m_input_rate}; //用一个输入采样的时长，也就是 1/m_input_rate 秒，作为时间单位。
  //av_rescale_q() 的作用是：保持同一个时间位置，将数值换成另一套单位。
  const auto samplePts = av_rescale_q(pts, inputTimeBase, inputSampleBase);
  // 容忍一个时间基 tick 或 1ms 内的封装时间戳偏差。
  // 始终对比累计采样位置，不逐帧重新锚定，因此不会无限容忍累计漂移。
  /*
  av_rescale_q_rnd(
    1,                 // 要换算的数值：1 个时间单位
    inputTimeBase,     // 原单位
    inputSampleBase,   // 目标单位：1 / 输入采样率
    AV_ROUND_UP);      // 向正无穷方向取整
  */
  const auto tolerance = std::max<std::int64_t>((static_cast<int64_t>(m_input_rate) + 999) / 1000,
                                                av_rescale_q_rnd(1, inputTimeBase, inputSampleBase, AV_ROUND_UP));

  // 4.检查这一帧是否接得上上一帧
  if (m_has_timestamp && std::abs(static_cast<long double>(samplePts) - m_next_input_pts) > tolerance) {
    m_error = "音频时间戳不连续：实际=" + std::to_string(samplePts) +
              "，预期=" + std::to_string(m_next_input_pts) +
              "，容差=" + std::to_string(tolerance) + "（每秒 " + std::to_string(m_input_rate) +
              " 个采样）；媒体时间=" + std::to_string(pts * av_q2d(inputTimeBase)) + " 秒";
    return AVERROR_INVALIDDATA;
  }
  //决定本帧使用哪个输入起点来推进时间轴
  const auto expectedInput = m_has_timestamp ? m_next_input_pts : samplePts;
  //防止时间换算失败时得到的异常结果和后面计算下一帧起点时发生整数溢出
  if (samplePts == std::numeric_limits<std::int64_t>::min() ||
      expectedInput > std::numeric_limits<std::int64_t>::max() - input.nb_samples)
    return AVERROR(ERANGE);

  // 5.整理实际输入数据的地址
  // 为了读取输入，我们需要提供几个地址？
  // planar 有多个平面；packed 只有一个平面。extended_data 也能覆盖 >8 声道。
  /*
  API：av_sample_fmt_is_planar接收一个采样格式，判断数据是否采用声道分开存储的方式
  分开存储，就需要每个声道的地址；交错存储，只需要一个总数据地址。
  */
  const int planeCount = av_sample_fmt_is_planar(m_input_format) ? m_input_channels : 1;

  //检查数据地址表存在，创建一个能装下所需地址的 vector，检查每个平面地址，并复制到 vector 中。
  if (!input.extended_data) return AVERROR(EINVAL);
  std::vector<const uint8_t *>
      planes(planeCount);
  for (int i = 0; i < planeCount; ++i) {
    if (!input.extended_data[i]) return AVERROR(EINVAL);
    planes[i] = input.extended_data[i];
  }

  // 6.第一帧确定输出时间轴的起点
  if (!m_has_timestamp) {
    //保持首帧时间位置不变，然后将输入帧的时间单位换算成转化后帧的时间单位，GetOutputTimeBase获取转化后帧的时间单位。
    m_next_output_pts = av_rescale_q(pts, inputTimeBase, GetOutputTimeBase());
    //检查转化结果是否出错。
    if (m_next_output_pts == std::numeric_limits<std::int64_t>::min())
      return AVERROR(ERANGE);
  }

  // 7. 输入 PCM 进行转化并取得输出。返回 0 且 output 为空也是正常情况。
  ret = Convert(planes.data(), input.nb_samples, output);
  if (ret < 0) return ret;
  m_has_timestamp = true;
  //本帧起点＋本帧采样数
  m_next_input_pts = expectedInput + input.nb_samples;
  return 0;
}

//排空
int AudioPipeline::Drain(FramePtr &output) {
  //1.清空上次输出，检查转换器状态
  output.reset();
  if (!m_context || m_failed) return AVERROR(EINVAL);

  //2.标记为正在排空
  m_draining = true;
  if (m_drained) return AVERROR_EOF;
  if (!m_has_timestamp) {
    m_drained = true;
    return AVERROR_EOF;
  }

  // 3. 输入已结束，反复空输入取出尾部。与解码器 receive 不同，swr 用 0 表示无输出。
  const int ret = Convert(nullptr, 0, output);
  if (ret < 0) return ret;
  if (output) return 0;
  m_drained = true;
  return AVERROR_EOF;
}

AVRational AudioPipeline::GetOutputTimeBase() const {
  return m_context ? AVRational{1, m_output_config.m_sample_rate} : AVRational{0, 1};
}

void AudioPipeline::Close() {
  m_error.clear();
  // 5. 释放 swr 内部资源。这里不交付尾部数据；正常完成应先 Drain。
  swr_free(&m_context);
  m_input_rate = m_input_channels = 0;
  m_input_layout = 0;
  m_input_format = AV_SAMPLE_FMT_NONE;
  m_next_input_pts = m_next_output_pts = 0;
  m_has_timestamp = m_draining = m_drained = m_failed = false;
}
} // namespace PLAYER
