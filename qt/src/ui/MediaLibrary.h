#pragma once

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>

namespace PLAYER {
// 保存媒体索引与文件夹显示名称；不修改或删除磁盘上的媒体。
class MediaLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList files READ GetFiles NOTIFY FilesChanged)
    Q_PROPERTY(QVariantList folders READ GetFolders NOTIFY FilesChanged)
    Q_PROPERTY(int videoCount READ GetVideoCount NOTIFY FilesChanged)
public:
    explicit MediaLibrary(QObject* parent = nullptr);
    ~MediaLibrary() override;
    QVariantList GetFiles() const { return m_files; }
    QVariantList GetFolders() const;
    int GetVideoCount() const;
    Q_INVOKABLE void AddFiles(const QList<QUrl>& urls);
    Q_INVOKABLE QUrl AddFolder(const QUrl& folder);
    Q_INVOKABLE void RefreshFolder(const QUrl& folder);
    Q_INVOKABLE void RenameFolder(const QUrl& folder, const QString& name);
    Q_INVOKABLE void RemoveFolder(const QUrl& folder);
    Q_INVOKABLE QVariantList FilesForFolder(const QUrl& folder) const;
    Q_INVOKABLE void RemoveFile(const QUrl& url);
    Q_INVOKABLE void Clear();
    Q_INVOKABLE QString NameFor(const QUrl& url) const;

signals:
    void FilesChanged();

private:
    struct Folder {
        QUrl url;
        QString name;
        QVariantList files;
        quint64 generation = 0;
        bool scanning = false;
        bool available = true;
    };
    void RebuildFiles();
    void Save() const;
    QVariantList m_files;
    QVariantList m_manual_files;
    QList<Folder> m_folders;
    QList<QThread*> m_workers;
    quint64 m_generation = 0;
};
} // namespace PLAYER
