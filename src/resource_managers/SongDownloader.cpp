//
// Song downloads from BMS table difficulty services.
//

#include "SongDownloader.h"

#include "db/SqliteCppDb.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <spdlog/spdlog.h>

namespace resource_managers::downloads {

auto
formatPatternUrl(const QString& pattern, const QString& md5) -> QString
{
    return pattern.arg(md5);
}

auto
parseGingerMeta(const QByteArray& body) -> std::optional<QString>
{
    const auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const auto url =
      doc.object().value(QStringLiteral("downloadURL")).toString();
    if (url.isEmpty()) {
        return std::nullopt;
    }
    return url;
}

auto
parseKonmaiMeta(const QByteArray& body) -> std::optional<QString>
{
    const auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const auto object = doc.object();
    if (object.value(QStringLiteral("result")).toString() !=
        QStringLiteral("success")) {
        return std::nullopt;
    }
    const auto url = object.value(QStringLiteral("data"))
                       .toObject()
                       .value(QStringLiteral("song_url"))
                       .toString();
    if (url.isEmpty()) {
        return std::nullopt;
    }
    return url;
}

auto
fileNameFromContentDisposition(const QString& header, const QString& fallback)
  -> QString
{
    // Content-Disposition: attachment; filename="package.7z"
    const auto prefix = QStringLiteral("filename=");
    const auto index = header.indexOf(prefix, 0, Qt::CaseInsensitive);
    if (index == -1) {
        return fallback;
    }
    auto name = header.sliced(index + prefix.size()).trimmed();
    if (name.startsWith('"') && name.endsWith('"') && name.size() >= 2) {
        name = name.sliced(1, name.size() - 2);
    }
    // Never let a hostile header escape the downloads folder.
    name = name.sliced(name.lastIndexOf('/') + 1);
    name = name.sliced(name.lastIndexOf('\\') + 1);
    if (name.isEmpty()) {
        return fallback;
    }
    return name;
}

auto
defaultDownloadSources() -> QList<DownloadSource>
{
    return {
        { .name = gingerSourceName,
          .urlPattern =
            QStringLiteral("https://gingerrush.com/download/package/%1"),
          .needsMetaQuery = true },
        { .name = wriggleSourceName,
          .urlPattern =
            QStringLiteral("https://bms.wrigglebug.xyz/download/package/%1"),
          .needsMetaQuery = false },
        { .name = konmaiSourceName,
          .urlPattern =
            QStringLiteral("https://bms.alvorna.com/api/hash?md5=%1"),
          .needsMetaQuery = true },
    };
}

} // namespace resource_managers::downloads

namespace {

// libarchive extraction of one package into the downloads folder.
// Runs on a worker thread; only plain return values cross threads.
// File contents stream through QFile so Unicode paths keep working on
// Windows, where libarchive's narrow filename APIs are codepage-bound.
auto
extractArchive(const QString& archivePath,
               const QString& destDir,
               QString& error) -> bool
{
    auto* reader = archive_read_new();
    archive_read_support_format_all(reader);
    archive_read_support_filter_all(reader);
#ifdef _WIN32
    const auto archiveName = archivePath.toStdWString();
    const auto opened =
      archive_read_open_filename_w(reader, archiveName.c_str(), 10240);
#else
    const auto archiveName = archivePath.toUtf8();
    const auto opened =
      archive_read_open_filename(reader, archiveName.constData(), 10240);
#endif
    if (opened != ARCHIVE_OK) {
        error = QString::fromUtf8(archive_error_string(reader));
        archive_read_free(reader);
        return false;
    }
    auto ok = true;
    archive_entry* entry = nullptr;
    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
        const auto relative = QString::fromUtf8(archive_entry_pathname(entry));
        // Defense in depth: never let a hostile archive escape the
        // downloads folder (alongside the Content-Disposition sanitizing).
        const auto clean = QDir::cleanPath(relative);
        if (clean.isEmpty() || QDir::isAbsolutePath(clean) ||
            clean == QStringLiteral("..") ||
            clean.startsWith(QStringLiteral("../"))) {
            spdlog::warn("Skipping suspicious archive entry: {}",
                         relative.toStdString());
            archive_read_data_skip(reader);
            continue;
        }
        const auto full = destDir + '/' + clean;
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            QDir{}.mkpath(full);
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG) {
            archive_read_data_skip(reader);
            continue;
        }
        QFileInfo{ full }.dir().mkpath(QStringLiteral("."));
        auto out = QFile{ full };
        if (!out.open(QIODevice::WriteOnly)) {
            error = QStringLiteral("Could not write %1").arg(full);
            ok = false;
            break;
        }
        auto failed = false;
        while (true) {
            auto buffer = std::array<char, 8192>{};
            const auto read =
              archive_read_data(reader, buffer.data(), buffer.size());
            if (read == 0) {
                break;
            }
            if (read < 0) {
                failed = true;
                break;
            }
            if (out.write(buffer.data(), read) != read) {
                failed = true;
                break;
            }
        }
        out.close();
        if (failed) {
            error = QString::fromUtf8(archive_error_string(reader));
            if (error.isEmpty()) {
                error = QStringLiteral("Could not write %1").arg(full);
            }
            ok = false;
            break;
        }
    }
    archive_read_close(reader);
    archive_read_free(reader);
    return ok;
}

} // namespace

resource_managers::SongDownloader::SongDownloader(
  QNetworkAccessManager* networkManager,
  const QDir& downloadLocation,
  db::SqliteCppDb* db,
  QObject* parent)
  : QAbstractListModel(parent)
  , networkManager(networkManager)
  , downloadLocation(downloadLocation)
  , db(db)
  , ownedCheck(
      db->createStatement("SELECT COUNT(*) FROM charts WHERE md5 = :md5"))
{
    fileOperationThreadPool.setMaxThreadCount(1);
    if (!downloadLocation.mkpath(".")) {
        spdlog::error("Failed to create folder for song downloads: {}",
                      downloadLocation.path().toStdString());
    }
    // Drop QSaveFile scratch files orphaned by an earlier crash or kill;
    // no download is in flight this early, so nothing here is live.
    static const auto stale =
      QRegularExpression{ QStringLiteral("\\.7z\\.[A-Za-z0-9]{6}$") };
    for (const auto& leftover :
         downloadLocation.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (stale.match(leftover).hasMatch()) {
            spdlog::info("Removing stale partial download: {}",
                         leftover.toStdString());
            QFile::remove(downloadLocation.filePath(leftover));
        }
    }
}

auto
resource_managers::SongDownloader::rowCount(const QModelIndex& parent) const
  -> int
{
    if (parent.isValid()) {
        return 0;
    }
    return tasks.size();
}

auto
resource_managers::SongDownloader::data(const QModelIndex& index,
                                        const int role) const -> QVariant
{
    if (!index.isValid() || index.row() < 0 || index.row() >= tasks.size()) {
        return {};
    }
    if (role == Qt::DisplayRole) {
        return QVariant::fromValue(tasks[index.row()]);
    }
    return {};
}

auto
resource_managers::SongDownloader::findRowByUrl(const QString& url) const -> int
{
    for (auto i = 0; i < tasks.size(); ++i) {
        if (tasks[i].url == url) {
            return i;
        }
    }
    return -1;
}

auto
resource_managers::SongDownloader::isOwned(const QString& md5) -> bool
{
    ownedCheck.reset();
    ownedCheck.bind(":md5", md5.toUpper().toStdString());
    const auto count = ownedCheck.executeAndGet<int64_t>();
    return count && *count > 0;
}

void
resource_managers::SongDownloader::notifyRow(int row)
{
    emit dataChanged(createIndex(row, 0), createIndex(row, 0));
}

auto
resource_managers::SongDownloader::submitMd5(const QString& md5,
                                             const QString& title) -> bool
{
    const auto normalized = md5.toUpper().trimmed();
    if (normalized.isEmpty()) {
        return false;
    }
    if (isOwned(normalized)) {
        spdlog::info("Skipping download of {}: already in the library",
                     normalized.toStdString());
        return false;
    }
    for (const auto& task : tasks) {
        if (task.md5 == normalized && task.status != DownloadTask::Error) {
            spdlog::info("Skipping download of {}: already queued",
                         normalized.toStdString());
            return false;
        }
    }
    beginInsertRows(QModelIndex(), tasks.size(), tasks.size());
    tasks.push_back(DownloadTask{ .id = nextId++,
                                  .name = title.isEmpty() ? normalized : title,
                                  .md5 = normalized });
    phases.push_back(Phase::ResolvePending);
    endInsertRows();
    emit countChanged();
    pump();
    return true;
}

void
resource_managers::SongDownloader::submitEntries(const QVariantList& items)
{
    for (const auto& item : items) {
        // Only table entries carry an md5; folder paths and owned charts
        // must never become download tasks.
        if (item.canView<Entry>()) {
            const auto entry = item.value<Entry>();
            submitMd5(entry.md5, entry.title);
        }
    }
}

void
resource_managers::SongDownloader::retry(int row)
{
    if (row < 0 || row >= tasks.size()) {
        return;
    }
    auto& task = tasks[row];
    if (task.status != DownloadTask::Error) {
        return;
    }
    task.status = DownloadTask::Prepare;
    task.errorMessage.clear();
    task.bytesReceived = 0;
    task.totalBytes = 0;
    if (!task.archivePath.isEmpty()) {
        QFile::remove(task.archivePath);
        task.archivePath.clear();
    }
    phases[row] = Phase::ResolvePending;
    notifyRow(row);
    pump();
}

auto
resource_managers::SongDownloader::availableSources() const -> QStringList
{
    auto names = QStringList{};
    for (const auto& source : downloads::defaultDownloadSources()) {
        names.push_back(source.name);
    }
    return names;
}

auto
resource_managers::SongDownloader::getSource() const -> QString
{
    return currentSource;
}

void
resource_managers::SongDownloader::setSource(const QString& source)
{
    if (source == currentSource) {
        return;
    }
    const auto known = availableSources().contains(source);
    if (!known) {
        spdlog::warn("Unknown song download source: {}", source.toStdString());
        return;
    }
    currentSource = source;
    emit sourceChanged();
}

void
resource_managers::SongDownloader::pump()
{
    if (networkBusy) {
        return;
    }
    // Extractions run on the worker pool; only start new network work when
    // no extraction is active so downloads stay strictly serial.
    for (const auto phase : phases) {
        if (phase == Phase::DownloadActive || phase == Phase::ExtractActive) {
            return;
        }
    }
    for (auto row = 0; row < tasks.size(); ++row) {
        if (phases[row] == Phase::ResolvePending) {
            startResolve(row);
            return;
        }
    }
    for (auto row = 0; row < tasks.size(); ++row) {
        if (phases[row] == Phase::DownloadQueued) {
            startDownload(row);
            return;
        }
    }
}

void
resource_managers::SongDownloader::startResolve(int row)
{
    auto sources = downloads::defaultDownloadSources();
    const auto it = std::ranges::find(
      sources, currentSource, &downloads::DownloadSource::name);
    if (it == sources.end()) {
        failTask(row, tr("Unknown download source"));
        pump();
        return;
    }
    const auto metaUrl =
      downloads::formatPatternUrl(it->urlPattern, tasks[row].md5);
    if (!it->needsMetaQuery) {
        tasks[row].url = metaUrl;
        if (const auto dupe = findRowByUrl(metaUrl);
            dupe != -1 && dupe != row &&
            tasks[dupe].status != DownloadTask::Error) {
            spdlog::info("Rejecting download task {}: already submitted",
                         metaUrl.toStdString());
            // A duplicate is not worth retrying, but it reuses the task slot
            // so the list stays append-only and row indices stay stable.
            failTask(row, tr("Already submitted"));
            pump();
            return;
        }
        phases[row] = Phase::DownloadQueued;
        pump();
        return;
    }
    networkBusy = true;
    auto* reply = networkManager->get(QNetworkRequest(QUrl(metaUrl)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, row] {
        reply->deleteLater();
        networkBusy = false;
        if (row < 0 || row >= tasks.size() ||
            phases[row] != Phase::ResolvePending) {
            pump();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            failTask(row, reply->errorString());
            pump();
            return;
        }
        auto url = std::optional<QString>{};
        if (currentSource == downloads::konmaiSourceName) {
            url = downloads::parseKonmaiMeta(reply->readAll());
        } else {
            url = downloads::parseGingerMeta(reply->readAll());
        }
        if (!url) {
            failTask(row, tr("Package not found at %1").arg(currentSource));
            pump();
            return;
        }
        tasks[row].url = *url;
        if (const auto dupe = findRowByUrl(*url);
            dupe != -1 && dupe != row &&
            tasks[dupe].status != DownloadTask::Error) {
            spdlog::info("Rejecting download task {}: already submitted",
                         url->toStdString());
            tasks[row].status = DownloadTask::Error;
            tasks[row].errorMessage = tr("Already submitted");
            // Keep the list append-only so row indices stay stable.
            phases[row] = Phase::Done;
            notifyRow(row);
            pump();
            return;
        }
        phases[row] = Phase::DownloadQueued;
        notifyRow(row);
        pump();
    });
}

void
resource_managers::SongDownloader::startDownload(int row)
{
    networkBusy = true;
    tasks[row].status = DownloadTask::Downloading;
    phases[row] = Phase::DownloadActive;
    notifyRow(row);
    // Stream straight to disk: song packages are far too large to buffer.
    activeFile = std::make_unique<QSaveFile>(
      downloadLocation.filePath(tasks[row].md5 + QStringLiteral(".7z")));
    if (!activeFile->open(QIODevice::WriteOnly)) {
        activeFile.reset();
        networkBusy = false;
        failTask(row, tr("Could not write download"));
        pump();
        return;
    }
    auto* reply = networkManager->get(QNetworkRequest(QUrl(tasks[row].url)));
    const auto writeFailed = std::make_shared<bool>(false);
    connect(reply,
            &QNetworkReply::downloadProgress,
            this,
            [this, row](qint64 received, qint64 total) {
                if (row < 0 || row >= tasks.size()) {
                    return;
                }
                tasks[row].bytesReceived = received;
                tasks[row].totalBytes = total;
                notifyRow(row);
            });
    connect(reply, &QNetworkReply::readyRead, this, [reply, this, writeFailed] {
        if (activeFile && activeFile->write(reply->readAll()) == -1) {
            *writeFailed = true;
            reply->abort();
        }
    });
    connect(
      reply, &QNetworkReply::finished, this, [this, reply, row, writeFailed] {
          reply->deleteLater();
          networkBusy = false;
          if (row < 0 || row >= tasks.size() ||
              phases[row] != Phase::DownloadActive) {
              activeFile.reset();
              pump();
              return;
          }
          if (reply->error() != QNetworkReply::NoError || *writeFailed ||
              activeFile->error() != QFileDevice::NoError) {
              activeFile->cancelWriting();
              activeFile.reset();
              failTask(row,
                       reply->error() != QNetworkReply::NoError &&
                           reply->error() !=
                             QNetworkReply::OperationCanceledError
                         ? reply->errorString()
                         : tr("Could not write download"));
              pump();
              return;
          }
          // Prefer the server's file name, like the reference implementation.
          const auto announced = downloads::fileNameFromContentDisposition(
            reply->header(QNetworkRequest::ContentDispositionHeader).toString(),
            tasks[row].md5 + QStringLiteral(".7z"));
          auto archivePath = activeFile->fileName();
          if (!activeFile->commit()) {
              activeFile.reset();
              failTask(row, tr("Could not write download"));
              pump();
              return;
          }
          activeFile.reset();
          const auto announcedPath = downloadLocation.filePath(announced);
          if (announcedPath != archivePath &&
              QFile::rename(archivePath, announcedPath)) {
              archivePath = announcedPath;
          }
          tasks[row].status = DownloadTask::Downloaded;
          tasks[row].archivePath = archivePath;
          phases[row] = Phase::ExtractQueued;
          notifyRow(row);
          startExtraction(row, archivePath);
          pump();
      });
}

void
resource_managers::SongDownloader::startExtraction(int row,
                                                   const QString& archivePath)
{
    tasks[row].status = DownloadTask::Extracting;
    phases[row] = Phase::ExtractActive;
    notifyRow(row);
    fileOperationThreadPool.start(
      [this, row, archivePath, dest = downloadLocation.path()] {
          auto error = QString{};
          const auto ok = extractArchive(archivePath, dest, error);
          QMetaObject::invokeMethod(
            this,
            [this, row, archivePath, ok, error] {
                finishExtraction(row, ok, error);
            },
            Qt::QueuedConnection);
      });
}

void
resource_managers::SongDownloader::finishExtraction(int row,
                                                    bool ok,
                                                    const QString& error)
{
    if (row < 0 || row >= tasks.size() || phases[row] != Phase::ExtractActive) {
        pump();
        return;
    }
    // The archive is only useful until its contents are extracted.
    if (!tasks[row].archivePath.isEmpty()) {
        QFile::remove(tasks[row].archivePath);
        tasks[row].archivePath.clear();
    }
    if (!ok) {
        failTask(row, error.isEmpty() ? tr("Extraction failed") : error);
        pump();
        return;
    }
    tasks[row].status = DownloadTask::Finished;
    phases[row] = Phase::Done;
    notifyRow(row);
    emit extractionFinished(downloadLocation.path());
    pump();
}

void
resource_managers::SongDownloader::failTask(int row, const QString& error)
{
    if (row < 0 || row >= tasks.size()) {
        return;
    }
    tasks[row].status = DownloadTask::Error;
    tasks[row].errorMessage = error.isEmpty() ? tr("Download failed") : error;
    phases[row] = Phase::Done;
    spdlog::error("Song download failed for {}: {}",
                  tasks[row].md5.toStdString(),
                  tasks[row].errorMessage.toStdString());
    notifyRow(row);
}
