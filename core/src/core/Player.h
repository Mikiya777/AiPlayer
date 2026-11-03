#pragma once
#include "audio/AudioRenderer.h"
#include "sync/AVSynchronizer.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
namespace PLAYER {
// 播放控制器：主线程调度和控制设备，两个工作线程独立提供音频/视频。
// Open/Tick/SetPaused/Close 只在主线程调用。当前支持本地文件、正常播放/暂停/停止。
class Player {
public:
    explicit Player(std::unique_ptr<IAudioOutput> audioOutput);
    ~Player();
    int Open(const std::string& path);
    int Seek(double seconds);
    // 每次返回至多一帧应显示的 RGBA；0=继续，EOF=音视频均播放结束。
    int Tick(TimedFrame& video);
    int SetPaused(bool paused);
    int SetVolume(double volume);
    int SetPlaybackRate(double rate);
    double GetPlaybackRate() const { return m_playback_rate; }
    void Close();
    bool IsPaused() const { return m_paused; }
    double GetPosition() const { return m_position; } // 原始媒体时间，秒。
    double GetDuration() const { return m_duration; }
    bool HasAudio() const { return m_has_audio; }
    bool HasVideo() const { return m_has_video; }
    std::string GetError() const;
    std::size_t GetPresentedCount() const { return m_presented; }
    std::size_t GetDroppedCount() const { return m_dropped; }
private:
    int OpenAt(const std::string& path, double target);
    void ProduceAudio(std::string path, AudioOutputConfig config, uint64_t generation, double target, double rate);
    void ProduceVideo(std::string path, uint64_t generation, double target);
    void Fail(const std::string& message);
    int FailCode(int code, const char* operation);
    void CheckCancelled() const;
    double ExternalTime() const;
    double AudioMediaTime(double deviceTime) const;
    void AnchorExternal(double time);

    AudioRenderer m_audio;
    AVSynchronizer m_sync;
    std::thread m_audio_thread, m_video_thread;
    std::atomic<bool> m_cancelled{false}, m_audio_done{true}, m_video_done{true};
    std::atomic<bool> m_failed{false};
    mutable std::mutex m_info_mutex;
    std::optional<double> m_first_video;
    std::string m_error;
    std::string m_path;
    uint64_t m_sync_generation = 0;
    bool m_has_audio = false, m_has_video = false, m_opened = false;
    bool m_started = false, m_audio_started = false, m_audio_finished = false;
    bool m_input_ended = false, m_paused = false, m_finished = false;
    double m_anchor_media = 0, m_position = 0, m_last_video_end = 0;
    double m_duration = 0, m_volume = 0.65, m_requested_start = 0;
    double m_playback_rate = 1.0;
    std::chrono::steady_clock::time_point m_anchor_wall;
    std::size_t m_presented = 0, m_dropped = 0;
};
} // namespace PLAYER
