#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantList>

namespace PLAYER {
// 轻量本地媒体列表；只保存用户本次运行中添加的文件，不接管文件本身。
class MediaLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList files READ GetFiles NOTIFY FilesChanged)
public:
    explicit MediaLibrary(QObject* parent = nullptr) : QObject(parent) {}
    QVariantList GetFiles() const { return m_files; }
    Q_INVOKABLE void AddFiles(const QList<QUrl>& urls);
    Q_INVOKABLE void AddFolder(const QUrl& folder);
    Q_INVOKABLE void RemoveFile(const QUrl& url);
    Q_INVOKABLE void Clear();
    Q_INVOKABLE QString NameFor(const QUrl& url) const;

signals:
    void FilesChanged();

private:
    void AddFile(const QString& path);
    QVariantList m_files;
};
} // namespace PLAYER
