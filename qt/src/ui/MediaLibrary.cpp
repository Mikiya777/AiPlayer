#include "ui/MediaLibrary.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <memory>

namespace PLAYER {
namespace {
const QSet<QString> sc_video_extensions = {
    "mp4", "mkv", "mov", "m4v", "avi", "webm", "ts", "mts", "m2ts", "mpg", "mpeg", "flv", "wmv"
};
QUrl LocalUrl(const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return QUrl::fromLocalFile(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}
QVariantMap FileEntry(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return {};
    return {{"url", LocalUrl(path)}, {"title", info.completeBaseName()},
            {"extension", info.suffix().toUpper()}, {"folder", info.absolutePath()},
            {"size", QLocale().formattedDataSize(info.size())}};
}
void RemoveEntry(QVariantList& files, const QUrl& url)
{
    files.erase(std::remove_if(files.begin(), files.end(), [&](const QVariant& entry) {
        return entry.toMap().value("url").toUrl() == url;
    }), files.end());
}
}

MediaLibrary::MediaLibrary(QObject* parent) : QObject(parent)
{
    QSettings settings("AiPlayer", "MediaLibrary");
    for (const auto& entry : settings.value("folders").toList()) {
        const auto map = entry.toMap();
        const QUrl url = map.value("url").toUrl();
        if (url.isLocalFile()) m_folders.push_back({url, map.value("name").toString()});
    }
    for (const auto& value : settings.value("files").toList()) {
        const QUrl url = value.toUrl();
        if (!url.isLocalFile()) continue;
        const auto entry = FileEntry(url.toLocalFile());
        if (!entry.isEmpty()) m_manual_files.push_back(entry);
    }
    RebuildFiles();
    QTimer::singleShot(0, this, [this] {
        const auto folders = m_folders;
        for (const auto& folder : folders) RefreshFolder(folder.url);
    });
}

MediaLibrary::~MediaLibrary()
{
    for (auto* worker : m_workers) worker->requestInterruption();
    for (auto* worker : m_workers) { worker->wait(); delete worker; }
}

QVariantList MediaLibrary::GetFolders() const
{
    QVariantList result;
    for (const auto& folder : m_folders)
        result.push_back(QVariantMap{{"url", folder.url}, {"name", folder.name},
            {"path", folder.url.toLocalFile()}, {"videoCount", folder.files.size()},
            {"scanning", folder.scanning}, {"available", folder.available}});
    return result;
}

int MediaLibrary::GetVideoCount() const
{
    QSet<QUrl> urls;
    for (const auto& folder : m_folders)
        for (const auto& file : folder.files) urls.insert(file.toMap().value("url").toUrl());
    return urls.size();
}

void MediaLibrary::RebuildFiles()
{
    QSet<QUrl> seen;
    m_files.clear();
    const auto append = [&](const QVariantList& files) {
        for (const auto& file : files) {
            const auto url = file.toMap().value("url").toUrl();
            if (!seen.contains(url)) { seen.insert(url); m_files.push_back(file); }
        }
    };
    append(m_manual_files);
    for (const auto& folder : m_folders) append(folder.files);
    emit FilesChanged();
}

void MediaLibrary::Save() const
{
    QSettings settings("AiPlayer", "MediaLibrary");
    QVariantList folders, files;
    for (const auto& folder : m_folders)
        folders.push_back(QVariantMap{{"url", folder.url}, {"name", folder.name}});
    for (const auto& file : m_manual_files) files.push_back(file.toMap().value("url"));
    settings.setValue("folders", folders);
    settings.setValue("files", files);
}

void MediaLibrary::AddFiles(const QList<QUrl>& urls)
{
    for (const auto& url : urls) {
        if (!url.isLocalFile()) continue;
        if (QFileInfo(url.toLocalFile()).isDir()) { AddFolder(url); continue; }
        const auto entry = FileEntry(url.toLocalFile());
        if (entry.isEmpty()) continue;
        const auto actual = entry.value("url").toUrl();
        // 从文件夹播放已有条目时，不额外变成脱离文件夹的独立记录。
        const bool exists = std::any_of(m_files.cbegin(), m_files.cend(), [&](const QVariant& file) {
            return file.toMap().value("url").toUrl() == actual;
        });
        if (!exists) { m_manual_files.push_back(entry); RebuildFiles(); }
    }
    Save();
}

QUrl MediaLibrary::AddFolder(const QUrl& url)
{
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isDir()) return {};
    const auto actual = LocalUrl(url.toLocalFile());
    for (const auto& folder : m_folders) if (folder.url == actual) return actual;
    QString name = QFileInfo(actual.toLocalFile()).fileName();
    if (name.isEmpty()) name = QDir::toNativeSeparators(actual.toLocalFile());
    m_folders.push_back({actual, name});
    Save();
    RefreshFolder(actual);
    return actual;
}

void MediaLibrary::RefreshFolder(const QUrl& url)
{
    auto folder = std::find_if(m_folders.begin(), m_folders.end(), [&](const Folder& f) { return f.url == url; });
    if (folder == m_folders.end() || folder->scanning) return;
    folder->generation = ++m_generation;
    folder->scanning = true;
    const auto generation = folder->generation;
    const QString path = url.toLocalFile();
    struct ScanResult { QVariantList files; bool available = false; };
    auto result = std::make_shared<ScanResult>();
    auto* worker = QThread::create([path, result] {
        const QFileInfo info(path);
        result->available = info.isDir() && info.isReadable();
        if (!result->available) return;
        QDirIterator iterator(path, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        QSet<QUrl> seen;
        while (iterator.hasNext()) {
            if (QThread::currentThread()->isInterruptionRequested()) return;
            const auto entryPath = iterator.next();
            if (!sc_video_extensions.contains(QFileInfo(entryPath).suffix().toLower())) continue;
            const auto entry = FileEntry(entryPath);
            if (entry.isEmpty()) continue;
            const auto fileUrl = entry.value("url").toUrl();
            if (!seen.contains(fileUrl)) { seen.insert(fileUrl); result->files.push_back(entry); }
        }
        std::sort(result->files.begin(), result->files.end(), [](const QVariant& a, const QVariant& b) {
            return a.toMap().value("url").toUrl().toString() < b.toMap().value("url").toUrl().toString();
        });
    });
    m_workers.push_back(worker);
    connect(worker, &QThread::finished, this, [this, worker, url, generation, result] {
        m_workers.removeOne(worker);
        worker->deleteLater();
        for (auto& folder : m_folders) {
            // 移除、重新添加或清空后的旧结果不允许重新插回媒体库。
            if (folder.url != url || folder.generation != generation) continue;
            folder.files = std::move(result->files);
            folder.available = result->available;
            folder.scanning = false;
            RebuildFiles();
            break;
        }
    }, Qt::QueuedConnection);
    emit FilesChanged();
    worker->start();
}

QVariantList MediaLibrary::FilesForFolder(const QUrl& url) const
{
    for (const auto& folder : m_folders) if (folder.url == url) return folder.files;
    return {};
}

void MediaLibrary::RenameFolder(const QUrl& url, const QString& name)
{
    if (name.trimmed().isEmpty()) return;
    for (auto& folder : m_folders) if (folder.url == url) {
        folder.name = name.trimmed(); Save(); emit FilesChanged(); return;
    }
}

void MediaLibrary::RemoveFolder(const QUrl& url)
{
    m_folders.erase(std::remove_if(m_folders.begin(), m_folders.end(), [&](const Folder& folder) {
        return folder.url == url;
    }), m_folders.end());
    Save(); RebuildFiles();
}

void MediaLibrary::RemoveFile(const QUrl& url)
{
    RemoveEntry(m_manual_files, url);
    for (auto& folder : m_folders) RemoveEntry(folder.files, url);
    Save(); RebuildFiles();
}

QString MediaLibrary::NameFor(const QUrl& url) const
{
    for (const auto& entry : m_files) {
        const auto map = entry.toMap();
        if (map.value("url").toUrl() == url) return map.value("title").toString();
    }
    return QFileInfo(url.toLocalFile()).completeBaseName();
}

void MediaLibrary::Clear()
{
    m_manual_files.clear(); m_folders.clear(); Save(); RebuildFiles();
}
} // namespace PLAYER
