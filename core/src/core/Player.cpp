#include "diagnostics/Performance.h"
#include "core/Player.h"
#include "reader/FileReader.h"
#include "video/VideoPipeline.h"
#include "audio/AudioTempo.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
extern "C" {
#include <libavutil/error.h>
}
namespace PLAYER {
namespace {
void Check(int result, const char *operation) {
  if (result >= 0) return;
  char error[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(result, error, sizeof(error));
  throw std::runtime_error(std::string(operation) + ": " + error);
}
void RetryLater() {
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
}
double FrameTime(const AVFrame &frame, AVRational base) {
  const int64_t pts = frame.best_effort_timestamp != AV_NOPTS_VALUE
                          ? frame.best_effort_timestamp
                          : frame.pts;
  return pts == AV_NOPTS_VALUE ? -1.0 : pts * av_q2d(base);
}
} // namespace
Player::Player(std::unique_ptr<IAudioOutput> audioOutput) : m_audio(std::move(audioOutput)) {}
Player::~Player() {
  Close();
}

int Player::Open(const std::string &path) {
  return OpenAt(path, 0.0);
}

int Player::OpenAt(const std::string &path, double target) {
  Close();
  {
    std::lock_guard<std::mutex> lock(m_info_mutex);
    m_error.clear();
    m_first_video.reset();
  }
  m_failed = false;
  m_cancelled = false;
  m_sync_generation = m_sync.Reset();
  m_started = m_audio_started = m_input_ended = m_paused = m_finished = false;
  m_requested_start = target;
  m_position = m_last_video_end = target;
  m_presented = m_dropped = 0;
  try {
    // 当前双读取器架构用于可重复读取的本地文件；不把网络输入当成普通文件。
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("请选择存在的本地媒体文件");
    Demuxer probe;
    Check(probe.Open(path), "探测输入");
    m_duration = probe.GetDurationSeconds();
    target = std::clamp(target, 0.0, m_duration > 0 ? m_duration : target);
    m_requested_start = target;
    m_position = m_last_video_end = target;
    m_has_audio = probe.GetBestStream(AVMEDIA_TYPE_AUDIO) >= 0;
    m_has_video = probe.GetBestStream(AVMEDIA_TYPE_VIDEO) >= 0;
    if (!m_has_audio && !m_has_video) throw std::runtime_error("输入没有可播放的音视频流");
    if (m_has_audio) {
      Check(m_audio.Open(), "打开音频设备");
      Check(m_audio.SetVolume(m_volume), "设置音量");
    }
    m_audio_done = !m_has_audio;
    m_video_done = !m_has_video;
    m_audio_finished = !m_has_audio;
    m_opened = true;
    // 两个读取器各自仅解码一种媒体，避免单一 ReadAll 回调因视频满队列
    // 堵住后续音频，从而发生“视频等音频时钟，音频又等视频出队”的死锁。
    // 代价是本地文件被解封装两次；后续可替换成单 demux + 独立 packet 队列。
    if (m_has_audio)
      m_audio_thread = std::thread(&Player::ProduceAudio, this, path,
                                   m_audio.GetOutputConfig(), m_audio.GetGeneration(), target, m_playback_rate);
    if (m_has_video)
      m_video_thread = std::thread(&Player::ProduceVideo, this, path, m_sync_generation, target);
    m_path = path;
    return 0;
  } catch (const std::exception &error) {
    Fail(error.what());
    Close();
    return AVERROR(EIO);
  }
}

int Player::Seek(double seconds) {
  if (!m_opened || !std::isfinite(seconds) || seconds < 0.0) return AVERROR(EINVAL);
  const bool paused = m_paused;
  const std::string path = m_path;
  const int ret = OpenAt(path, std::clamp(seconds, 0.0, m_duration));
  if (ret < 0) return ret;
  return paused ? SetPaused(true) : 0;
}
void Player::CheckCancelled() const {
  if (m_cancelled.load()) throw std::runtime_error("播放已停止");
}
void Player::Fail(const std::string &message) {
  if (m_cancelled.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(m_info_mutex);
    m_error = message;
  }
  m_failed.store(true);
}
int Player::FailCode(int code, const char *operation) {
  if (code >= 0) return code;
  char error[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, error, sizeof(error));
  Fail(std::string(operation) + ": " + error);
  return code;
}
std::string Player::GetError() const {
  std::lock_guard<std::mutex> lock(m_info_mutex);
  return m_error;
}
void Player::ProduceAudio(std::string path, AudioOutputConfig config, uint64_t generation, double target, double rate) {
  try {
    FileReader reader;
    Check(reader.Open(path, true, false), "打开音频流");
    if (target > 0) Check(reader.Seek(target), "定位音频流");
    AudioPipeline pipeline;
    AudioTempo tempo;
    if (rate != 1.0) Check(tempo.Open(config, rate), "初始化保音调变速");
    bool opened = false;
    auto Submit = [&](const FramePtr &frame) {
      if (!frame) return;
      for (;;) {
        CheckCancelled();
        const int ret = m_audio.Enqueue(*frame, pipeline.GetOutputTimeBase(), generation);
        if (ret != AVERROR(EAGAIN)) {
          Check(ret, "音频入队");
          break;
        }
        RetryLater();
      }
    };
    auto ReceiveTempo = [&](bool draining) {
      FramePtr output;
      int ret;
      while ((ret = tempo.Receive(output)) == 0) Submit(output);
      if (ret != (draining ? AVERROR_EOF : AVERROR(EAGAIN))) Check(ret, "读取变速 PCM");
    };
    auto ProcessTempo = [&](const FramePtr &pcm) {
      if (!pcm) return;
      if (rate == 1.0) {
        Submit(pcm);
        return;
      }
      Check(tempo.Send(pcm.get()), "音频变速");
      ReceiveTempo(false);
    };
    Check(reader.ReadAll([&](const AVFrame &frame, AVRational base) {
      CheckCancelled();
      // seek 会落到目标之前的关键点；旧帧只用于恢复解码状态，不进入播放队列。
      const double time = FrameTime(frame, base);
      if (time >= 0 && time < target) return;
      if (!opened) {
        Check(pipeline.Open(frame, config), "初始化 swr");
        opened = true;
      }
      FramePtr pcm;
      const int converted = pipeline.Process(frame, base, pcm);
      if (converted < 0 && !pipeline.GetError().empty())
        throw std::runtime_error("PCM 转换：" + pipeline.GetError());
      Check(converted, "PCM 转换");
      ProcessTempo(pcm);
    },
                         {}, &m_cancelled),
          "音频解码");
    if (opened) {
      FramePtr pcm;
      int ret;
      while ((ret = pipeline.Drain(pcm)) == 0) ProcessTempo(pcm);
      if (ret != AVERROR_EOF) Check(ret, "排空 swr");
    }
    if (rate != 1.0) {
      Check(tempo.Send(nullptr), "排空变速处理器");
      ReceiveTempo(true);
    }
    Check(m_audio.EndInput(generation), "音频输入结束");
  } catch (const std::exception &error) {
    if (!m_cancelled.load()) Fail(error.what());
  }
  m_audio_done.store(true);
}
void Player::ProduceVideo(std::string path, uint64_t generation, double target) {
  try {
    FileReader reader;
    Check(reader.Open(path, false, true), "打开视频流");
    if (target > 0) Check(reader.Seek(target), "定位视频流");
    VideoPipeline pipeline;
    Check(reader.ReadAll({}, [&](const AVFrame &frame, AVRational base) {
            CheckCancelled();
            const double time = FrameTime(frame, base);
            if (time >= 0 && time < target) return;
            FramePtr rgba;
            Check(pipeline.Process(frame, rgba), "RGBA 转换");
            // RGBA 自己持有像素内存和时间戳，可以安全离开解码回调。
            const double waitStart=Performance::Get().Active()?Performance::Now():0;
            for (;;) {
                CheckCancelled();
                const int ret = m_sync.PushVideoFrame(*rgba, base, generation);
                if (ret != AVERROR(EAGAIN)) { Check(ret, "视频入队"); break; }
                RetryLater();
            }
            if(waitStart)Performance::Get().Add("video_producer_backpressure",Performance::Now()-waitStart);
            std::lock_guard<std::mutex> lock(m_info_mutex);
            if (!m_first_video) {
                const auto pts = rgba->pts != AV_NOPTS_VALUE ? rgba->pts : rgba->best_effort_timestamp;
                m_first_video = pts * av_q2d(base);
            } }, &m_cancelled), "视频解码");
  } catch (const std::exception &error) {
    if (!m_cancelled.load()) Fail(error.what());
  }
  m_video_done.store(true);
}
void Player::AnchorExternal(double time) {
  m_anchor_media = time;
  m_anchor_wall = std::chrono::steady_clock::now();
}
double Player::ExternalTime() const {
  return m_anchor_media + m_playback_rate * std::chrono::duration<double>(std::chrono::steady_clock::now() - m_anchor_wall).count();
}
double Player::AudioMediaTime(double deviceTime) const {
  const auto start = m_audio.GetStartTime().value_or(m_requested_start);
  // 2× 时设备播放 1 秒 PCM，对应原媒体前进 2 秒。首帧偏移不能乘倍速。
  return start + (deviceTime - start) * m_playback_rate;
}
int Player::Tick(TimedFrame &video) {
  video = {};
  if (m_failed.load()) return AVERROR(EIO);
  if (!m_opened) return AVERROR(ECANCELED);
  if (m_finished) return AVERROR_EOF;
  if (m_paused) return 0;
  const auto firstAudio = m_has_audio ? m_audio.GetStartTime() : std::optional<double>{};
  std::optional<double> firstVideo;
  {
    std::lock_guard<std::mutex> lock(m_info_mutex);
    firstVideo = m_first_video;
  }
  if (!m_started) {
    // 先等两路首帧或 EOF，取共同起点；不能分别把两路第一帧强行改为0秒。
    if ((m_has_audio && !firstAudio && !m_audio_done.load()) ||
        (m_has_video && !firstVideo && !m_video_done.load())) return 0;
    m_position = firstAudio.value_or(firstVideo.value_or(m_requested_start));
    if (firstVideo) m_position = std::min(m_position, *firstVideo);
    m_position = std::max(m_position, m_requested_start);
    m_last_video_end = m_position;
    AnchorExternal(m_position);
    m_started = true;
  }
  if (!m_audio_started || m_audio_finished) m_position = ExternalTime();
  // 音频比视频晚开始时，先按外部时钟显示前导视频，到音频起点才开始供给设备。
  if (m_has_audio && !m_audio_finished && (!firstAudio || m_position >= *firstAudio)) {
    m_audio_started = true;
    const int ret = m_audio.Pump();
    if (auto audioTime = m_audio.GetPlaybackTime()) m_position = AudioMediaTime(*audioTime);
    if (ret == AVERROR_EOF) {
      m_audio_finished = true;
      // 音频较短时，剩余视频从最后音频位置接续，不会永远等待冻结的时钟。
      AnchorExternal(m_position);
    } else if (ret < 0) {
      Fail("音频输出设备发生错误");
      return ret;
    }
  }
  if(Performance::Get().Active())Performance::Get().m_audio_media_time.store(m_position,std::memory_order_relaxed);
  // 有音频时使用音频进度；纯视频/前导视频/音频尾后使用上述明确的外部时钟。
  int ret = m_sync.UpdateAudioClock(m_position, m_sync_generation);
  if (ret < 0) return FailCode(ret, "更新主时钟");
  if (!m_input_ended && m_audio_done.load() && m_video_done.load()) {
    ret = m_sync.EndInput(m_sync_generation);
    if (ret < 0) return FailCode(ret, "同步器输入结束");
    m_input_ended = true;
  }
  auto decision = m_sync.PollVideo();
  m_dropped += decision.m_dropped_frames;
  if (decision.m_action == VideoAction::kPresent) {
    m_last_video_end = decision.m_video.m_time_seconds + decision.m_video.m_duration_seconds;
    video = std::move(decision.m_video);
    ++m_presented;
  }
  if (m_input_ended && m_audio_finished && m_sync.IsDrained() && m_position >= m_last_video_end) {
    m_finished = true;
    return AVERROR_EOF;
  }
  return 0;
}
int Player::SetPaused(bool paused) {
  if (m_failed.load()) return AVERROR(EIO);
  if (!m_opened || m_finished) return AVERROR(ECANCELED);
  if (m_paused == paused) return 0;
  if (m_has_audio && !m_audio_finished) {
    const int ret = m_audio.SetPaused(paused);
    if (ret < 0) {
      FailCode(ret, "暂停/恢复音频设备");
      Close(); // 设备已停止，取消并回收两路生产线程，不能继续使用旧队列。
      return ret;
    }
    if (paused && m_audio_started) {
      if (auto time = m_audio.GetPlaybackTime()) m_position = AudioMediaTime(*time);
    }
  }
  if (paused && m_started && (!m_audio_started || m_audio_finished)) m_position = ExternalTime();
  if (!paused) AnchorExternal(m_position); // 暂停的墙钟时间不能算进媒体进度。
  m_sync.SetPaused(paused);
  m_paused = paused;
  return 0;
}

int Player::SetVolume(double volume) {
  if (!std::isfinite(volume) || volume < 0.0 || volume > 1.0) return AVERROR(EINVAL);
  m_volume = volume;
  return m_opened && m_has_audio ? m_audio.SetVolume(volume) : 0;
}
int Player::SetPlaybackRate(double rate) {
  if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0) return AVERROR(EINVAL);
  if (rate == m_playback_rate) return 0;
  // 从当前媒体位置重建队列，避免设备里旧倍速 PCM 与新映射混用。
  const bool opened = m_opened, paused = m_paused;
  const double position = m_position;
  const auto path = m_path;
  Close(); // 先 join 生产线程，再更改速率。
  m_playback_rate = rate;
  if (!opened) return 0;
  const int ret = OpenAt(path, position);
  return ret < 0 ? ret : (paused ? SetPaused(true) : 0);
}
void Player::Close() {
  m_cancelled.store(true);
  m_audio.Stop();
  if (m_audio_thread.joinable()) m_audio_thread.join();
  if (m_video_thread.joinable()) m_video_thread.join();
  m_sync.Reset();
  m_opened = false;
}
} // namespace PLAYER
