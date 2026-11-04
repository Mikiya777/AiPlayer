#pragma once
#include "core/MediaTypes.h"
#include "effects/FaceStickerFilter.h"
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QPointer>
#include <QString>
#include <vector>

namespace PLAYER {
// OpenGL 3.3 Core 渲染器，不负责解码或决定显示时刻。
// 所有调用必须在同一个 OpenGL context 为 current 的线程进行。
// 窗口负责提供 context/FBO，在销毁 context 前调用 Release。
class VideoRenderer : protected QOpenGLFunctions_3_3_Core {
public:
    bool Initialize();
    bool Upload(const AVFrame& rgba);
    bool Draw(int framebufferWidth, int framebufferHeight);
    void SetFilters(bool flip, bool gray, bool invert) { m_flip=flip; m_gray=gray; m_invert=invert; }
    void SetFaces(std::vector<FaceStickerPose> faces) { m_faces=std::move(faces); }
    // 清屏并释放大图存储；false 仅用于旧保留策略的内存对照。
    void Clear(bool releaseStorage = true);
    void Release();
    QString GetError() const { return m_error; }
private:
    bool CheckContext();
    QPointer<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    GLuint m_vao = 0, m_vbo = 0, m_texture = 0;
    int m_width = 0, m_height = 0;
    double m_display_aspect = 1;
    std::vector<uint8_t> m_packed_pixels;
    QString m_error;
    bool m_flip=false, m_gray=false, m_invert=false;
    GLuint m_sticker=0;
    std::vector<FaceStickerPose> m_faces;
};
} // namespace PLAYER
