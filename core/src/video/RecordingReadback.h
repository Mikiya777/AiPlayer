#pragma once
#include "core/MediaTypes.h"
#include "diagnostics/Performance.h"
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <array>
#include <cstring>
#include <limits>
namespace PLAYER {
// 调用者已绑定单采样录制 FBO。只读回颜色，不包含 Qt 控件。
inline FramePtr ReadRecordingFrame(int width, int height) {
  auto *context = QOpenGLContext::currentContext();
  if (!context || width <= 0 || height <= 0) return {};
  FramePtr frame(av_frame_alloc());
  if (!frame) return {};
  frame->format = AV_PIX_FMT_RGBA;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame.get(), 32) < 0) return {};
  auto *gl = context->extraFunctions();
  GLint alignment, rowLength, skipRows, skipPixels, buffer, readBuffer;
  gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
  gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
  gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
  gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
  gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
  gl->glGetIntegerv(GL_READ_BUFFER, &readBuffer);
  gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
  gl->glPixelStorei(GL_PACK_ROW_LENGTH, frame->linesize[0] / 4);
  gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
  {
    PerformanceScope measure("record_direct_readpixels");
    gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame->data[0]);
  }
  const GLenum error = gl->glGetError();
  gl->glReadBuffer(readBuffer);
  gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  gl->glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
  gl->glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
  gl->glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
  gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
  if (error != GL_NO_ERROR) return {};
  // OpenGL 自底向上；用负行跨度提供顶部起点，buf[] 仍持有原始分配。
  frame->data[0] += static_cast<ptrdiff_t>(height - 1) * frame->linesize[0];
  frame->linesize[0] = -frame->linesize[0];
  return frame;
}
// 三个有界 PBO：只有 fence 已完成才映射，绝不等待 GPU 完成当前帧。
// 必须在创建它的 GL 上下文线程上使用和销毁。
class RecordingPboReadback {
public:
  RecordingPboReadback() = default;
  RecordingPboReadback(const RecordingPboReadback &) = delete;
  RecordingPboReadback &operator=(const RecordingPboReadback &) = delete;
  ~RecordingPboReadback() {
    Reset();
  }

  void Reset() {
    if (auto *context = QOpenGLContext::currentContext()) {
      auto *gl = context->extraFunctions();
      for (auto &slot : m_slots) {
        if (slot.fence) gl->glDeleteSync(slot.fence);
        if (slot.buffer) gl->glDeleteBuffers(1, &slot.buffer);
      }
    }
    m_slots = {};
    m_head = m_count = 0;
  }
  bool Pending() const {
    return m_count != 0;
  }

  bool Enqueue(int width, int height, double time) {
    if (m_count == m_slots.size()) {
      Performance::Get().Add("record_pbo_skipped_count", 1);
      return true; // 满时跳过采集，不扩大缓存或阻塞播放。
    }
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() / 4 ||
        size_t(height) > size_t(std::numeric_limits<GLsizeiptr>::max()) / (size_t(width) * 4))
      return false;
    auto *gl = QOpenGLContext::currentContext()->extraFunctions();
    auto &slot = m_slots[(m_head + m_count) % m_slots.size()];
    PackState state(gl);
    if (!slot.buffer) gl->glGenBuffers(1, &slot.buffer);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.buffer);
    const GLsizeiptr bytes = GLsizeiptr(width) * 4 * height;
    if (slot.bytes != bytes) {
      gl->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
      slot.bytes = bytes;
    }
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    {
      PerformanceScope measure("record_pbo_enqueue");
      gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      slot.fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      gl->glFlush();
    }
    if (!slot.fence || gl->glGetError() != GL_NO_ERROR) {
      if (slot.fence) gl->glDeleteSync(slot.fence);
      slot.fence = nullptr;
      return false;
    }
    slot.width = width;
    slot.height = height;
    slot.time = time;
    ++m_count;
    return true;
  }

  FramePtr Take(double &time, bool &failed) {
    failed = false;
    if (!m_count) return {};
    auto *gl = QOpenGLContext::currentContext()->extraFunctions();
    auto &slot = m_slots[m_head];
    const auto status = gl->glClientWaitSync(slot.fence, 0, 0);
    if (status == GL_TIMEOUT_EXPIRED) return {};
    if (status == GL_WAIT_FAILED) {
      failed = true;
      return {};
    }
    PerformanceScope measure("record_pbo_map_copy");
    FramePtr frame(av_frame_alloc());
    if (!frame) {
      failed = true;
      return {};
    }
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = slot.width;
    frame->height = slot.height;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
      failed = true;
      return {};
    }
    PackState state(gl);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.buffer);
    const auto *pixels = static_cast<const uint8_t *>(
        gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, slot.bytes, GL_MAP_READ_BIT));
    if (!pixels) {
      failed = true;
      return {};
    }
    for (int y = 0; y < slot.height; ++y)
      std::memcpy(frame->data[0] + ptrdiff_t(y) * frame->linesize[0],
                  pixels + ptrdiff_t(y) * slot.width * 4, size_t(slot.width) * 4);
    const bool valid = gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    time = slot.time; // 保留采集时刻，不能使用映射时刻。
    gl->glDeleteSync(slot.fence);
    slot.fence = nullptr;
    m_head = (m_head + 1) % m_slots.size();
    --m_count;
    if (!valid) {
      failed = true;
      return {};
    }
    frame->data[0] += ptrdiff_t(frame->height - 1) * frame->linesize[0];
    frame->linesize[0] = -frame->linesize[0];
    Performance::Get().Add("record_pbo_completed_count", 1);
    return frame;
  }

private:
  struct PackState {
    QOpenGLExtraFunctions *gl;
    GLint alignment, row, skipRows, skipPixels, buffer, read;
    explicit PackState(QOpenGLExtraFunctions *f) : gl(f) {
      gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
      gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
      gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
      gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
      gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
      gl->glGetIntegerv(GL_READ_BUFFER, &read);
    }
    ~PackState() {
      gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
      gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
      gl->glPixelStorei(GL_PACK_ROW_LENGTH, row);
      gl->glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
      gl->glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
      gl->glReadBuffer(read);
    }
  };
  struct Slot {
    GLuint buffer = 0;
    GLsync fence = nullptr;
    GLsizeiptr bytes = 0;
    int width = 0, height = 0;
    double time = 0;
  };
  std::array<Slot, 3> m_slots{};
  size_t m_head = 0, m_count = 0;
};
} // namespace PLAYER
