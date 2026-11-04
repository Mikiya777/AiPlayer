#include "diagnostics/Performance.h"
#include "video/VideoRenderer.h"
#include <QOpenGLContext>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <QImage>
#include <QPainter>
#include <QVector4D>
namespace PLAYER {
bool VideoRenderer::CheckContext()
{
    if (!m_context || QOpenGLContext::currentContext() != m_context) {
        m_error = QStringLiteral("OpenGL context 未绑定或不匹配");
        return false;
    }
    return true;
}
bool VideoRenderer::Initialize()
{
    if (m_program) return CheckContext();
    m_context = QOpenGLContext::currentContext();
    if (!m_context || !initializeOpenGLFunctions()) {
        m_error = QStringLiteral("需要 OpenGL 3.3 Core 或更高版本");
        return false;
    }
    m_program = std::make_unique<QOpenGLShaderProgram>();
    // 顶点矩形覆盖 viewport；UV 的 v 在上下方向翻转，匹配 CPU 图像首行在顶部。
    const char* vertex = R"(#version 330 core
layout(location=0) in vec2 position;
layout(location=1) in vec2 texCoord;
out vec2 uv;
void main() { gl_Position = vec4(position, 0.0, 1.0); uv = texCoord; }
)";
    const char* fragment = R"(#version 330 core
in vec2 uv;
uniform sampler2D videoTexture;
out vec4 color;
uniform bool flipImage, grayscale, invertColor;
uniform sampler2D stickerTexture;
uniform vec2 imageSize;
uniform int faceCount;
uniform vec4 faces[8];
void main() {
    vec2 source = vec2(uv.x, flipImage ? 1.0-uv.y : uv.y);
    vec3 rgb = texture(videoTexture, source).rgb;
    if (grayscale) rgb = vec3(dot(rgb, vec3(0.2126,0.7152,0.0722)));
    if (invertColor) rgb = vec3(1.0)-rgb;
    for (int i=0; i<faceCount; ++i) {
        vec2 p = source*imageSize-faces[i].xy;
        float c=cos(faces[i].w), s=sin(faces[i].w);
        vec2 local=vec2(c*p.x+s*p.y, -s*p.x+c*p.y);
        vec2 st=local/vec2(faces[i].z, faces[i].z*0.3)+0.5;
        if (all(greaterThanEqual(st,vec2(0))) && all(lessThanEqual(st,vec2(1)))) {
            vec4 decal=texture(stickerTexture,st);
            rgb=mix(rgb,decal.rgb,decal.a);
        }
    }
    color=vec4(rgb,1.0);
}
)";
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !m_program->link()) {
        m_error = m_program->log();
        Release();
        return false;
    }
    constexpr float sc_vertices[] = {-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0};
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sc_vertices), sc_vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    // 使用透明纹理绘制标牌，之后可替换为自定义 PNG。
    QImage sticker(512,154,QImage::Format_RGBA8888); sticker.fill(Qt::transparent);
    QPainter painter(&sticker); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#16664d"),10)); painter.setBrush(QColor("#e6ffb0"));
    painter.drawRoundedRect(QRectF(8,8,496,138),24,24);
    QFont font; font.setPixelSize(57); font.setBold(true); painter.setFont(font);
    painter.drawText(sticker.rect(),Qt::AlignCenter,QStringLiteral("★  主角登场  ★")); painter.end();
    glGenTextures(1,&m_sticker); glBindTexture(GL_TEXTURE_2D,m_sticker);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,sticker.width(),sticker.height(),0,GL_RGBA,GL_UNSIGNED_BYTE,sticker.constBits());
    glBindTexture(GL_TEXTURE_2D,0);
    return true;
}
bool VideoRenderer::Upload(const AVFrame& rgba)
{
    PerformanceScope measure("texture_upload_cpu");
    if (!CheckContext() || !m_program) return false;
    if (rgba.format != AV_PIX_FMT_RGBA || rgba.width <= 0 || rgba.height <= 0 ||
        !rgba.data[0] || std::abs(static_cast<int64_t>(rgba.linesize[0])) < static_cast<int64_t>(rgba.width) * 4) {
        m_error = QStringLiteral("渲染器需要有效的 RGBA 帧");
        return false;
    }
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (rgba.width > maxSize || rgba.height > maxSize) {
        m_error = QStringLiteral("视频尺寸超过设备纹理上限"); return false;
    }
    // 正行跨度可以直接上传，包括 FFmpeg 的行对齐填充。仅负跨度需要重排。
    const uint8_t* pixels=rgba.data[0];
    int rowLength=0;
    if(rgba.linesize[0]>0 && rgba.linesize[0]%4==0) {
        rowLength=rgba.linesize[0]/4;
    } else {
        const size_t rowBytes=static_cast<size_t>(rgba.width)*4;
        m_packed_pixels.resize(rowBytes*rgba.height);
        for(int y=0;y<rgba.height;++y)
            std::memcpy(m_packed_pixels.data()+y*rowBytes,
                rgba.data[0]+static_cast<ptrdiff_t>(y)*rgba.linesize[0],rowBytes);
        pixels=m_packed_pixels.data();
    }
    Performance::Get().Add("memory_upload_scratch_capacity_mib",m_packed_pixels.capacity()/1048576.0);
    Performance::Get().Add("memory_video_texture_nominal_mib",static_cast<double>(rgba.width)*rgba.height*4/1048576.0);
    GLint oldAlignment=0, oldRowLength=0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&oldAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH,&oldRowLength);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    if (m_width != rgba.width || m_height != rgba.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width, rgba.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_width = rgba.width; m_height = rgba.height;
    }
    // 相同分辨率只更新纹理内容，不逐帧重新创建纹理对象。
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRowLength);
    glBindTexture(GL_TEXTURE_2D, 0);
    const double sar = rgba.sample_aspect_ratio.num > 0 && rgba.sample_aspect_ratio.den > 0
                     ? av_q2d(rgba.sample_aspect_ratio) : 1.0;
    m_display_aspect = static_cast<double>(m_width) / m_height * sar;
    if (glGetError() != GL_NO_ERROR) { m_error = QStringLiteral("OpenGL 纹理上传失败"); return false; }
    return true;
}
bool VideoRenderer::Draw(int framebufferWidth, int framebufferHeight)
{
    PerformanceScope measure("draw_submit_cpu");
    if (!CheckContext() || !m_program || framebufferWidth <= 0 || framebufferHeight <= 0) return false;
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (m_width == 0) return true;
    // 保持显示宽高比（包括非方形像素），窗口剩余区域留黑边，不拉伸画面。
    int width = framebufferWidth, height = framebufferHeight;
    if (static_cast<double>(width) / height > m_display_aspect)
        width = std::max(1, static_cast<int>(height * m_display_aspect));
    else height = std::max(1, static_cast<int>(width / m_display_aspect));
    glViewport((framebufferWidth - width) / 2, (framebufferHeight - height) / 2, width, height);
    m_program->bind();
    m_program->setUniformValue("videoTexture", 0);
    m_program->setUniformValue("flipImage",m_flip);
    m_program->setUniformValue("grayscale",m_gray);
    m_program->setUniformValue("invertColor",m_invert);
    m_program->setUniformValue("imageSize",QVector2D(m_width,m_height));
    const int count=static_cast<int>(std::min<size_t>(8,m_faces.size()));
    m_program->setUniformValue("faceCount",count);
    for(int i=0;i<count;++i) {
        const auto& f=m_faces[i];
        m_program->setUniformValue(("faces["+std::to_string(i)+"]").c_str(),QVector4D(f.x,f.y,f.width,f.angle));
    }
    m_program->setUniformValue("stickerTexture",1);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,m_sticker);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_texture);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0); glBindTexture(GL_TEXTURE_2D, 0); m_program->release();
    if (glGetError() != GL_NO_ERROR) { m_error = QStringLiteral("OpenGL 绘制失败"); return false; }
    return true;
}
void VideoRenderer::Clear(bool releaseStorage)
{
    m_faces.clear();
    m_width = m_height = 0;
    if (releaseStorage) {
        std::vector<uint8_t>().swap(m_packed_pixels);
        if (m_texture && CheckContext()) {
            // 保留纹理对象/采样参数，但归还原视频的大图存储。
            // 后续 Upload 会按新的尺寸重新分配；操作仍在 GL 上下文线程。
            GLint previousTexture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
            glBindTexture(GL_TEXTURE_2D, m_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        }
    } else {
        m_packed_pixels.clear();
    }
}
void VideoRenderer::Release()
{
    if (!m_context || QOpenGLContext::currentContext() != m_context) return;
    if (m_sticker) glDeleteTextures(1,&m_sticker);
    m_sticker=0;
    if (m_texture) glDeleteTextures(1, &m_texture);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    m_texture = m_vbo = m_vao = 0;
    m_program.reset(); m_context.clear(); m_width = m_height = 0;
    std::vector<uint8_t>().swap(m_packed_pixels);
}
} // namespace PLAYER
