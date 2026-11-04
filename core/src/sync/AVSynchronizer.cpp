#include "diagnostics/Performance.h"
#include "sync/AVSynchronizer.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace PLAYER {
namespace {
// 最多建议等 10ms 就重新检查，便于响应暂停、停止和设备进度变化。
constexpr double sc_max_poll_wait = 0.010;
} // namespace

AVSynchronizer::AVSynchronizer(const SyncConfig &config)
    : m_config(config) {
  if (config.m_max_audio_frames == 0 || config.m_max_video_frames == 0 ||
      !std::isfinite(config.m_early_tolerance) || config.m_early_tolerance < 0 ||
      !std::isfinite(config.m_late_tolerance) || config.m_late_tolerance < 0 ||
      !std::isfinite(config.m_default_video_duration) || config.m_default_video_duration <= 0)
    throw std::invalid_argument("Invalid AVSynchronizer configuration");
}

std::uint64_t AVSynchronizer::GetGeneration() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_generation;
}

std::uint64_t AVSynchronizer::Reset(double commonOriginSeconds) {
  if (!std::isfinite(commonOriginSeconds))
    throw std::invalid_argument("Invalid media time origin");
  std::lock_guard<std::mutex> lock(m_mutex);
  m_audio_frames.clear();
  m_video_frames.clear();
  m_audio_clock.reset();
  m_last_audio_time.reset();
  m_last_video_time.reset();
  m_common_origin = commonOriginSeconds;
  m_paused = false;
  m_input_finished = false;
  return ++m_generation;
}

void AVSynchronizer::SetPaused(bool paused) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_paused = paused;
}

int AVSynchronizer::PushAudioFrame(const AVFrame &frame, AVRational timeBase,
                                   std::uint64_t generation) {
  return PushFrame(frame, timeBase, generation, true);
}

int AVSynchronizer::PushVideoFrame(const AVFrame &frame, AVRational timeBase,
                                   std::uint64_t generation) {
  return PushFrame(frame, timeBase, generation, false);
}

int AVSynchronizer::PushFrame(const AVFrame &frame, AVRational timeBase,
                              std::uint64_t generation, bool audio) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_generation) return AVERROR(ECANCELED);
  if (m_input_finished) return AVERROR_EOF;
  if (timeBase.num <= 0 || timeBase.den <= 0) return AVERROR(EINVAL);
  if (audio ? (frame.sample_rate <= 0 || frame.nb_samples <= 0)
            : (frame.width <= 0 || frame.height <= 0))
    return AVERROR(EINVAL);

  // 解码帧优先用 PTS，缺失时尝试 FFmpeg 推算出的呈现时间。
  const std::int64_t pts = frame.pts != AV_NOPTS_VALUE
                               ? frame.pts
                               : frame.best_effort_timestamp;
  if (pts == AV_NOPTS_VALUE) return AVERROR_INVALIDDATA;
  const double time = pts * av_q2d(timeBase) - m_common_origin;
  auto &lastTime = audio ? m_last_audio_time : m_last_video_time;
  if (!std::isfinite(time) || (lastTime && time < *lastTime))
    return AVERROR_INVALIDDATA;

  auto &queue = audio ? m_audio_frames : m_video_frames;
  const auto capacity = audio ? m_config.m_max_audio_frames : m_config.m_max_video_frames;
  if (queue.size() >= capacity) return AVERROR(EAGAIN);

  TimedFrame queued;
  queued.m_frame.reset(av_frame_clone(&frame));
  if (!queued.m_frame) return AVERROR(ENOMEM);
  queued.m_time_base = timeBase;
  queued.m_time_seconds = time;
  queued.m_generation = m_generation;
  queued.m_duration_seconds = audio
                                  ? static_cast<double>(frame.nb_samples) / frame.sample_rate
                                  : (frame.pkt_duration > 0 ? frame.pkt_duration * av_q2d(timeBase)
                                                            : m_config.m_default_video_duration);
  queued.m_profile_enqueue_ms=Performance::Get().Active()?Performance::Now():0;
  queue.push_back(std::move(queued));
  if(!audio) {
    Performance::Get().Add("video_queue_frames",queue.size());
    Performance::Get().Add("video_queue_capacity_frames",m_config.m_max_video_frames);
    if(Performance::Get().Active()) {
      double bytes=0;
      for(const auto& entry:queue)
        bytes+=std::abs(static_cast<int64_t>(entry.m_frame->linesize[0]))*entry.m_frame->height;
      Performance::Get().Add("memory_video_queue_payload_mib",bytes/1048576.0);
    }
  }
  lastTime = time;
  return 0;
}

int AVSynchronizer::PopAudioFrame(TimedFrame &output, std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_generation) return AVERROR(ECANCELED);
  if (m_audio_frames.empty()) return m_input_finished ? AVERROR_EOF : AVERROR(EAGAIN);
  if (m_paused) return AVERROR(EAGAIN);
  output = std::move(m_audio_frames.front());
  m_audio_frames.pop_front();
  return 0;
}

int AVSynchronizer::UpdateAudioClock(double mediaSeconds, std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_generation) return AVERROR(ECANCELED);
  const double time = mediaSeconds - m_common_origin;
  if (!std::isfinite(time)) return AVERROR(EINVAL);
  m_audio_clock = time;
  return 0;
}

int AVSynchronizer::UpdateAudioClockFromBuffer(double submittedEndSeconds,
                                               double pendingSeconds,
                                               std::uint64_t generation) {
  if (!std::isfinite(submittedEndSeconds) || !std::isfinite(pendingSeconds) || pendingSeconds < 0)
    return AVERROR(EINVAL);
  return UpdateAudioClock(submittedEndSeconds - pendingSeconds, generation);
}

VideoDecision AVSynchronizer::PollVideo() {
  std::lock_guard<std::mutex> lock(m_mutex);
  VideoDecision result;
  result.m_generation = m_generation;
  if (m_video_frames.empty()) {
    result.m_action = m_input_finished ? VideoAction::kFinished : VideoAction::kEmpty;
    return result;
  }
  if (m_paused) {
    result.m_action = VideoAction::kPaused;
    return result;
  }
  if (!m_audio_clock) {
    result.m_action = VideoAction::kWaitingForClock;
    return result;
  }

  // 追赶只丢“已解码的过时画面”。至少保留一帧，且下一帧也已到显示时刻，
  // 才跳过当前帧，避免丢完后没有可显示的画面。绝不在这里丢压缩包。
  while (m_video_frames.size() > 1) {
    const auto &current = m_video_frames[0];
    const auto &next = m_video_frames[1];
    const double interval = next.m_time_seconds - current.m_time_seconds;
    const double duration = interval > 0 ? interval : current.m_duration_seconds;
    const double lateThreshold = std::max(m_config.m_late_tolerance, duration);
    const bool stale = *m_audio_clock - current.m_time_seconds > lateThreshold;
    const bool nextReady = next.m_time_seconds <= *m_audio_clock + m_config.m_early_tolerance;
    if (!stale || !nextReady) break;
    m_video_frames.pop_front();
    ++result.m_dropped_frames;
    Performance::Get().Add("video_dropped_count",1);
  }

  result.m_difference_seconds = m_video_frames.front().m_time_seconds - *m_audio_clock;
  if (result.m_difference_seconds > m_config.m_early_tolerance) {
    result.m_action = VideoAction::kWait;
    result.m_wait_seconds = std::min(sc_max_poll_wait,
                                     result.m_difference_seconds - m_config.m_early_tolerance);
    return result;
  }

  Performance::Get().Add("schedule_video_minus_audio",result.m_difference_seconds*1000);
  if(m_video_frames.front().m_profile_enqueue_ms)Performance::Get().Add("video_queue_wait",Performance::Now()-m_video_frames.front().m_profile_enqueue_ms);
  result.m_action = VideoAction::kPresent;
  result.m_video = std::move(m_video_frames.front());
  m_video_frames.pop_front();
  return result;
}

int AVSynchronizer::EndInput(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_generation) return AVERROR(ECANCELED);
  m_input_finished = true;
  return 0;
}

bool AVSynchronizer::IsDrained() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_input_finished && m_audio_frames.empty() && m_video_frames.empty();
}
} // namespace PLAYER
