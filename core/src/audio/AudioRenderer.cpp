#include "audio/AudioRenderer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace PLAYER {
AudioRenderer::AudioRenderer(std::unique_ptr<IAudioOutput> output)
    : m_output(std::move(output)), m_owner_thread(std::this_thread::get_id()) {
  if (!m_output) throw std::invalid_argument("音频输出设备不能为空");
}

AudioRenderer::~AudioRenderer() {
  Stop();
}

bool AudioRenderer::IsOwnerThread() const {
  return std::this_thread::get_id() == m_owner_thread;
}

int AudioRenderer::Open(AudioOutputConfig requested) {
  if (!IsOwnerThread()) return AVERROR(EPERM);
  Stop();
  // 先协商设备格式，再创建重采样器。不能只更改格式标签却仍发送旧格式 PCM。
  int ret = m_output->Open(requested);
  if (ret < 0) {
    m_output->Stop();
    return ret;
  }
  const int channels = av_get_channel_layout_nb_channels(requested.m_channel_layout);
  const int sampleBytes = av_get_bytes_per_sample(requested.m_sample_format);
  if (requested.m_sample_rate <= 0 || channels < 1 || channels > 2 || sampleBytes <= 0 ||
      av_sample_fmt_is_planar(requested.m_sample_format)) {
    m_output->Stop();
    return AVERROR(EINVAL);
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = requested;
  m_bytes_per_frame = channels * sampleBytes;
  // 应用队列最多保存约一秒 PCM；设备内部另有约 100ms 缓冲。
  m_capacity_bytes = static_cast<std::int64_t>(requested.m_sample_rate) * m_bytes_per_frame;
  m_opened = true;
  return 0;
}

std::uint64_t AudioRenderer::GetGeneration() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_generation;
}

std::optional<double> AudioRenderer::GetStartTime() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_has_timestamp ? std::optional<double>{m_first_time} : std::nullopt;
}

int AudioRenderer::Enqueue(const AVFrame &frame, AVRational timeBase, std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_generation || !m_opened) return AVERROR(ECANCELED);
  if (m_input_finished) return AVERROR_EOF;
  if (frame.sample_rate != m_config.m_sample_rate || frame.format != m_config.m_sample_format ||
      frame.channel_layout != m_config.m_channel_layout ||
      frame.channels != av_get_channel_layout_nb_channels(m_config.m_channel_layout) ||
      frame.nb_samples <= 0 || !frame.data[0] || frame.pts == AV_NOPTS_VALUE ||
      timeBase.num <= 0 || timeBase.den <= 0) return AVERROR(EINVAL);
  const auto bytes = static_cast<std::int64_t>(frame.nb_samples) * m_bytes_per_frame;
  if (bytes > m_capacity_bytes) return AVERROR(EINVAL); // 超大帧应拆分，不能无限重试。
  if (bytes > m_capacity_bytes - m_queued_bytes || m_queue.size() >= 512)
    return AVERROR(EAGAIN);
  const auto pts = av_rescale_q(frame.pts, timeBase, AVRational{1, m_config.m_sample_rate});
  // 换算失败会返回 INT64_MIN；还需保证帧尾采样位置可以安全累加。
  // 在克隆和入队前拒绝，不改变时间轴，调用方仍可提交合法帧。
  if (pts == std::numeric_limits<std::int64_t>::min() ||
      pts > std::numeric_limits<std::int64_t>::max() - frame.nb_samples)
    return AVERROR(ERANGE);
  // 当前接口接 AudioPipeline 输出，时间基为 1/sample_rate，严格保持连续。
  // 遇到 seek/不连续的新片段，控制者应 Stop/Open，不能把两段无声拼接。
  if (m_has_timestamp && pts != m_next_pts) return AVERROR_INVALIDDATA;
  FramePtr copy(av_frame_clone(&frame));
  if (!copy) return AVERROR(ENOMEM);
  m_queue.push_back({std::move(copy), bytes, 0});
  if (!m_has_timestamp) {
    m_first_time = static_cast<double>(pts) / m_config.m_sample_rate;
    m_has_timestamp = true;
  }
  m_next_pts = pts + frame.nb_samples;
  m_queued_bytes += bytes;
  return 0;
}

int AudioRenderer::EndInput(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_generation || !m_opened) return AVERROR(ECANCELED);
  m_input_finished = true;
  return 0;
}

void AudioRenderer::RefreshClock() {
  if (!m_has_timestamp || m_submitted_bytes == 0) return;
  const double submitted = static_cast<double>(m_submitted_bytes / m_bytes_per_frame) /
                           m_config.m_sample_rate;
  const double processed = m_output->GetProcessedSeconds();
  if (!std::isfinite(processed)) return;
  // processedUSecs 统计已处理的音频，elapsedUSecs 会包含暂停/欠载时间，不能替代它。
  // 不把应用队列算进播放进度，也不让进度越过实际提交的数据末尾。
  const double time = m_first_time + std::clamp(processed, 0.0, submitted);
  m_clock = m_clock ? std::max(*m_clock, time) : time;
}

int AudioRenderer::Pump() {
  if (!IsOwnerThread()) return AVERROR(EPERM);
  std::unique_lock<std::mutex> lock(m_mutex);
  if (m_finished) return AVERROR_EOF;
  if (!m_opened) return AVERROR(ECANCELED);
  auto Fail = [&](int error) {
    lock.unlock();
    Stop(); // 清队列并更新代次，生产者下次入队就能感知取消。
    return error;
  };
  int ret = m_output->GetError();
  if (ret < 0) return Fail(ret);
  if (m_paused) return 0;
  // 每轮最多写设备当前空闲容量，避免生产者不断入队使主线程长时间不返回。
  auto budget = m_output->GetFreeBytes();
  while (budget > 0 && !m_queue.empty()) {
    auto &audio = m_queue.front();
    const auto requested = std::min(budget, audio.m_bytes - audio.m_offset);
    const auto written = m_output->Write(audio.m_frame->data[0] + audio.m_offset, requested);
    if (written < 0 || written > requested) return Fail(AVERROR(EIO));
    if (written == 0) break; // 等下一次定时器，不忙等。
    audio.m_offset += written;
    m_submitted_bytes += written;
    m_queued_bytes -= written;
    budget -= written;
    if (audio.m_offset == audio.m_bytes) m_queue.pop_front();
  }
  RefreshClock();
  // 三个条件缺一不可：生产完成、应用队列空、设备缓冲已消耗。
  // Idle 也可能只是临时缺数据，未 EndInput 时不能当作播放结束。
  if (m_input_finished && m_queue.empty() && m_output->IsIdle()) {
    m_output->Finish();
    m_finished = true;
    // 保留最后一次估算时钟；不要用“全部提交完成”伪造精确播放位置。
    return AVERROR_EOF;
  }
  return 0;
}

int AudioRenderer::SetPaused(bool paused) {
  if (!IsOwnerThread()) return AVERROR(EPERM);
  std::unique_lock<std::mutex> lock(m_mutex);
  if (!m_opened || m_finished) return AVERROR(ECANCELED);
  if (m_paused == paused) return 0;
  if (paused) RefreshClock();
  m_output->SetPaused(paused);
  const int ret = m_output->GetError();
  if (ret < 0) {
    // 设备状态已不可信：与 Pump 的失败路径保持一致，停止并取消生产者。
    // Stop 会再次加锁，必须先解锁；只在成功后提交 m_paused。
    lock.unlock();
    Stop();
    return ret;
  }
  m_paused = paused;
  return 0;
}

int AudioRenderer::SetVolume(double volume) {
  if (!IsOwnerThread() || !std::isfinite(volume) || volume < 0.0 || volume > 1.0)
    return AVERROR(EINVAL);
  m_output->SetVolume(volume);
  return m_output->GetError();
}

void AudioRenderer::ClearQueue() {
  m_queue.clear();
  m_queued_bytes = m_submitted_bytes = m_next_pts = 0;
  m_clock.reset();
  m_has_timestamp = m_input_finished = m_finished = m_paused = false;
}

void AudioRenderer::Stop() {
  // 与 Qt 对象一样，renderer 也必须在创建它的线程销毁。
  if (!IsOwnerThread()) std::terminate();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_output->Stop();
  m_opened = false;
  ++m_generation;
  ClearQueue();
}
} // namespace PLAYER
