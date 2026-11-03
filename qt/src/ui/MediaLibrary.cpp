#include "ui/MediaLibrary.h"
#include <QDirIterator>
#include <QFileInfo>
#include <QLocale>
#include <QSet>

namespace PLAYER {
namespace {
const QSet<QString> sc_media_extensions = {
    QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
    QStringLiteral("m4v"), QStringLiteral("avi"), QStringLiteral("webm"),
    QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("m4a"),
    QStringLiteral("wav"), QStringLiteral("aac"), QStringLiteral("ogg")
};
}

void MediaLibrary::AddFile(const QString& path)
{
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return;
    const QUrl url = QUrl::fromLocalFile(info.absoluteFilePath());
    for (const QVariant& entry : m_files)
        if (entry.toMap().value(QStringLiteral("url")).toUrl() == url) return;

    QVariantMap entry;
    entry.insert(QStringLiteral("url"), url);
    entry.insert(QStringLiteral("title"), info.completeBaseName());
    entry.insert(QStringLiteral("extension"), info.suffix().toUpper());
    entry.insert(QStringLiteral("folder"), info.absolutePath());
    entry.insert(QStringLiteral("size"), QLocale().formattedDataSize(info.size()));
    m_files.push_back(entry);
}

void MediaLibrary::AddFiles(const QList<QUrl>& urls)
{
    const qsizetype before = m_files.size();
    for (const QUrl& url : urls) if (url.isLocalFile()) AddFile(url.toLocalFile());
    if (m_files.size() != before) emit FilesChanged();
}

void MediaLibrary::AddFolder(const QUrl& folder)
{
    if (!folder.isLocalFile()) return;
    const qsizetype before = m_files.size();
    QDirIterator iterator(folder.toLocalFile(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (sc_media_extensions.contains(QFileInfo(path).suffix().toLower())) AddFile(path);
    }
    if (m_files.size() != before) emit FilesChanged();
}

void MediaLibrary::RemoveFile(const QUrl& url)
{
    for (qsizetype index = 0; index < m_files.size(); ++index) {
        if (m_files[index].toMap().value(QStringLiteral("url")).toUrl() == url) {
            m_files.removeAt(index);
            emit FilesChanged();
            return;
        }
    }
}

QString MediaLibrary::NameFor(const QUrl& url) const
{
    for (const QVariant& entry : m_files) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("url")).toUrl() == url)
            return map.value(QStringLiteral("title")).toString();
    }
    return QFileInfo(url.toLocalFile()).completeBaseName();
}

void MediaLibrary::Clear()
{
    if (m_files.isEmpty()) return;
    m_files.clear();
    emit FilesChanged();
}
} // namespace PLAYER
