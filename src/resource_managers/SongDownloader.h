//
// Song downloads from BMS table difficulty services.
//
// The download flow ports the in-game song downloader from
// seraxis/lr2oraja-endlessdream (building on beatoraja):
// an md5 from a table entry is resolved to a song package URL through a
// download service (Ginger, Wriggle, or Konmai), the archive is downloaded
// with progress reporting, extracted into the managed downloads folder, and
// the folder is rescanned so the new songs appear in the library.
//

#ifndef SONGDOWNLOADER_H
#define SONGDOWNLOADER_H

#include "Tables.h"
#include "db/SqliteCppDb.h"

#include <QAbstractListModel>
#include <QDir>
#include <QNetworkReply>
#include <QSaveFile>
#include <memory>
#include <optional>
#include <qthreadpool.h>

class QNetworkAccessManager;

namespace resource_managers {

namespace downloads {

/**
 * @brief Builds a download-service URL from a pattern holding one md5 slot.
 * @details Patterns use Qt `%1` placeholders, e.g.
 * `https://bms.wrigglebug.xyz/download/package/%1`. This mirrors the
 * `String.format(downloadURL, md5)` single-placeholder contract of the
 * reference `HttpDownloadSource` implementations.
 */
auto
formatPatternUrl(const QString& pattern, const QString& md5) -> QString;

/**
 * @brief Extracts the package URL from a Ginger meta-query response.
 * @details Ginger answers `GET <pattern>/<md5>` with
 * `{fileName, fileSize, downloadURL}`. Returns nullopt when the payload
 * carries no usable `downloadURL`.
 */
auto
parseGingerMeta(const QByteArray& body) -> std::optional<QString>;

/**
 * @brief Extracts the package URL from a Konmai meta-query response.
 * @details Konmai answers `GET <pattern>?md5=<md5>` with
 * `{result, msg, data: {song_url, ...}}`. Only `result == "success"` with a
 * non-empty `song_url` yields a URL; anything else is nullopt, mirroring the
 * reference `KonmaiDownloadSource` (empty `song_url` means "no such song").
 */
auto
parseKonmaiMeta(const QByteArray& body) -> std::optional<QString>;

/**
 * @brief Picks the archive file name for a finished download.
 * @details Prefers the `filename` from the `Content-Disposition` header and
 * falls back to `fallback` (normally `<md5>.7z`), mirroring the reference
 * `downloadFileFromURL` behavior.
 */
auto
fileNameFromContentDisposition(const QString& header,
                              const QString& fallback) -> QString;

struct DownloadSource
{
    QString name;
    QString urlPattern;
    // Ginger and Konmai resolve the md5 to a package URL through a JSON
    // meta-query first; Wriggle links the package directly.
    bool needsMetaQuery;
};

auto
defaultDownloadSources() -> QList<DownloadSource>;

inline const QString gingerSourceName = QStringLiteral("ginger");
inline const QString wriggleSourceName = QStringLiteral("wriggle");
inline const QString konmaiSourceName = QStringLiteral("konmai");

} // namespace downloads

struct DownloadTask
{
    Q_GADGET

  public:
    enum Status
    {
        Prepare,
        Downloading,
        Downloaded,
        Extracting,
        Finished,
        Error
    };
    Q_ENUM(Status)

  private:
    Q_PROPERTY(int id MEMBER id CONSTANT)
    Q_PROPERTY(QString name MEMBER name CONSTANT)
    Q_PROPERTY(QString md5 MEMBER md5 CONSTANT)
    Q_PROPERTY(QString url MEMBER url)
    Q_PROPERTY(Status status MEMBER status)
    Q_PROPERTY(qint64 bytesReceived MEMBER bytesReceived)
    Q_PROPERTY(qint64 totalBytes MEMBER totalBytes)
    Q_PROPERTY(QString errorMessage MEMBER errorMessage)

  public:
    int id{};
    QString name;
    QString md5;
    QString url;
    Status status{ Prepare };
    qint64 bytesReceived{ 0 };
    qint64 totalBytes{ 0 };
    QString errorMessage;
    // Local archive path once downloaded; empty before that.
    QString archivePath;
};

/**
 * @brief Downloads missing table songs through md5-keyed package services.
 * @details Tasks move through Prepare -> Downloading -> Downloaded ->
 * Extracting -> Finished (or Error). Downloads and extractions run strictly
 * one at a time: the reference implementation notes that concurrent downloads
 * can lock the song database, so the queue is serial here. Emits
 * extractionFinished() after each successful extraction so the caller can
 * register and rescan the downloads folder.
 */
class SongDownloader final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString source READ getSource WRITE setSource NOTIFY
                 sourceChanged)
    Q_PROPERTY(int count READ getCount NOTIFY countChanged)

    QNetworkAccessManager* networkManager;
    QDir downloadLocation;
    db::SqliteCppDb* db;
    db::SqliteCppDb::Statement ownedCheck;
    QList<DownloadTask> tasks;
    // Internal queue phase per task, parallel to tasks by row.
    enum class Phase
    {
        ResolvePending,
        DownloadQueued,
        DownloadActive,
        ExtractQueued,
        ExtractActive,
        Done
    };
    QList<Phase> phases;
    int nextId{ 0 };
    QString currentSource = downloads::gingerSourceName;
    QThreadPool fileOperationThreadPool;
    bool networkBusy{ false };
    // Scratch file for the single in-flight download; song packages stream
    // straight to disk instead of buffering in memory.
    std::unique_ptr<QSaveFile> activeFile;

    void pump();
    void startResolve(int row);
    void startDownload(int row);
    void startExtraction(int row, const QString& archivePath);
    void finishExtraction(int row, bool ok, const QString& error);
    void failTask(int row, const QString& error);
    [[nodiscard]] auto findRowByUrl(const QString& url) const -> int;
    [[nodiscard]] auto isOwned(const QString& md5) -> bool;
    void notifyRow(int row);

  public:
    explicit SongDownloader(QNetworkAccessManager* networkManager,
                            const QDir& downloadLocation,
                            db::SqliteCppDb* db,
                            QObject* parent = nullptr);
    auto rowCount(const QModelIndex& parent = QModelIndex()) const
      -> int override;
    auto data(const QModelIndex& index, int role = Qt::DisplayRole) const
      -> QVariant override;
    /**
     * @brief Queues a download for a single table-entry md5.
     * @details Songs already present in the library and md5s already queued
     * are skipped. Mirrors the reference `submitMD5Task` dedup behavior.
     * @return Whether a new task was queued.
     */
    Q_INVOKABLE auto submitMd5(const QString& md5, const QString& title)
      -> bool;
    /**
     * @brief Queues downloads for every missing song in a level listing.
     * @details Accepts the mixed `ChartData*`/`Entry` list returned by
     * `Level::loadCharts()` (or `Level::entries`); owned charts and plain
     * md5 strings are handled, everything else is ignored. Mirrors the
     * reference "Fill Missing Charts" action.
     */
    Q_INVOKABLE void submitEntries(const QVariantList& items);
    /**
     * @brief Retries a failed task from the failed row.
     */
    Q_INVOKABLE void retry(int row);
    /**
     * @brief Names of the configured download services.
     */
    Q_INVOKABLE QStringList availableSources() const;
    auto getSource() const -> QString;
    void setSource(const QString& source);
    auto getCount() const -> int { return tasks.size(); }

  signals:
    void sourceChanged();
    void countChanged();
    /**
     * @brief Emitted with the downloads folder path after an extraction.
     * @details The caller is expected to register the folder as a song root
     * and rescan it; this mirrors the reference `main.updateSong(dir, true)`
     * step without coupling the downloader to the folder configuration.
     */
    void extractionFinished(const QString& downloadDirPath);
};

} // namespace resource_managers

#endif // SONGDOWNLOADER_H
