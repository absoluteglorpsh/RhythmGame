#include "resource_managers/SongDownloader.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

using namespace resource_managers::downloads;

TEST_CASE("wriggle source builds a direct package URL from the md5")
{
    const auto sources = defaultDownloadSources();
    const auto it =
      std::ranges::find(sources, wriggleSourceName, &DownloadSource::name);
    REQUIRE(it != sources.end());
    CHECK_FALSE(it->needsMetaQuery);
    CHECK(formatPatternUrl(it->urlPattern, "ABC123") ==
          "https://bms.wrigglebug.xyz/download/package/ABC123");
}

TEST_CASE("ginger is the default source and needs a meta query")
{
    const auto sources = defaultDownloadSources();
    REQUIRE_FALSE(sources.empty());
    CHECK(sources.front().name == gingerSourceName);
    const auto it =
      std::ranges::find(sources, gingerSourceName, &DownloadSource::name);
    REQUIRE(it != sources.end());
    CHECK(it->needsMetaQuery);
    CHECK(formatPatternUrl(it->urlPattern, "abc") ==
          "https://gingerrush.com/download/package/abc");
}

TEST_CASE("konmai source queries its hash endpoint")
{
    const auto sources = defaultDownloadSources();
    const auto it =
      std::ranges::find(sources, konmaiSourceName, &DownloadSource::name);
    REQUIRE(it != sources.end());
    CHECK(it->needsMetaQuery);
    CHECK(formatPatternUrl(it->urlPattern, "abc") ==
          "https://bms.alvorna.com/api/hash?md5=abc");
}

TEST_CASE("ginger meta response yields the package URL")
{
    const auto body =
      QByteArray{ R"({"fileName":"pack.7z","fileSize":42,)"
                  R"("downloadURL":"https://cdn.example/pack.7z"})" };
    const auto url = parseGingerMeta(body);
    REQUIRE(url.has_value());
    CHECK(*url == "https://cdn.example/pack.7z");
}

TEST_CASE("ginger meta response without a URL yields nothing")
{
    CHECK_FALSE(parseGingerMeta("{}").has_value());
    CHECK_FALSE(parseGingerMeta("not json").has_value());
    CHECK_FALSE(parseGingerMeta(R"({"downloadURL":""})").has_value());
}

TEST_CASE("konmai meta response yields the song URL on success")
{
    const auto body = QByteArray{
        R"({"result":"success","msg":"","chart":"x",)"
        R"("data":{"chart_name":"c","md5":"m","sha256":"s",)"
        R"("song_name":"song","song_url":"https://cdn.example/s.7z"}})"
    };
    const auto url = parseKonmaiMeta(body);
    REQUIRE(url.has_value());
    CHECK(*url == "https://cdn.example/s.7z");
}

TEST_CASE("konmai meta response without success yields nothing")
{
    CHECK_FALSE(parseKonmaiMeta(R"({"result":"fail","msg":"nope","data":{}})")
                  .has_value());
    CHECK_FALSE(
      parseKonmaiMeta(R"({"result":"success","data":{"song_url":""}})")
        .has_value());
    CHECK_FALSE(parseKonmaiMeta("not json").has_value());
}

TEST_CASE("content disposition filename is preferred over the fallback")
{
    CHECK(fileNameFromContentDisposition(R"(attachment; filename="pack.7z")",
                                         "ABC.7z") == "pack.7z");
    CHECK(fileNameFromContentDisposition("", "ABC.7z") == "ABC.7z");
    CHECK(fileNameFromContentDisposition("attachment", "ABC.7z") == "ABC.7z");
}

TEST_CASE("content disposition cannot escape the downloads folder")
{
    CHECK(fileNameFromContentDisposition(
            R"(attachment; filename="../../evil.7z")", "ABC.7z") == "evil.7z");
    CHECK(fileNameFromContentDisposition(
            R"(attachment; filename="..\\evil.7z")", "ABC.7z") == "evil.7z");
}
