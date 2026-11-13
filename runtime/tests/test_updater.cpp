#include "check.h"
#include <updater.h>

TEST(updater_compare_versions)
{
    CHECK(updater::CompareVersions("0.2.0", "0.1.9") > 0);
    CHECK(updater::CompareVersions("v0.10.0", "0.9.1") > 0);
    CHECK(updater::CompareVersions("1.0", "1.0.0") == 0);
    CHECK(updater::CompareVersions("0.1.0", "v0.1.0") == 0);
    CHECK(updater::CompareVersions("0.1.0-rc1", "0.1.0") == 0);
    CHECK(updater::CompareVersions("0.1.0", "0.1.1") < 0);
}

TEST(updater_json_string)
{
    const std::string json = R"({"url":"x","author":{"login":"someone"},"tag_name": "v0.2.0","name":"First \"public\" build","draft":false,
        "assets":[{"name":"SCDA_Setup.exe"}]})";
    CHECK_EQ(updater::JsonString(json, "tag_name"), std::string("v0.2.0"));
    CHECK_EQ(updater::JsonString(json, "name"), std::string("First \"public\" build"));
    CHECK_EQ(updater::JsonString(json, "body"), std::string());
    CHECK_EQ(updater::JsonString(json, "draft"), std::string()); // not a string
}

// Trimmed GitLab release object (field order as gitlab.com sends it; the author and commit objects
// also hold "name" keys, after the release's own).
TEST(updater_parse_gitlab_release)
{
    const std::string json = R"({"name":"TCSCC Recomp 0.2.0","tag_name":"v0.2.0","description":"notes","created_at":"2026-11-07",
        "author":{"id":1,"username":"x","name":"Someone"},"commit":{"id":"abc","title":"t","author_name":"Someone"},
        "assets":{"count":3,"sources":[{"format":"zip","url":"https://gitlab.com/g/p/-/archive/v0.2.0/p-v0.2.0.zip"}],
        "links":[{"id":1,"name":"TCSCC_Setup.zip","url":"https://gitlab.com/-/project/1/uploads/a/TCSCC_Setup.zip","direct_asset_url":"https://gitlab.com/g/p/-/releases/v0.2.0/downloads/TCSCC_Setup.zip","link_type":"other"},
                 {"id":2,"name":"TCSCC_Setup.exe","url":"https://gitlab.com/-/project/1/uploads/b/TCSCC_Setup.exe","direct_asset_url":"https://gitlab.com/g/p/-/releases/v0.2.0/downloads/TCSCC_Setup.exe","link_type":"other"}]},
        "_links":{"self":"https://gitlab.com/g/p/-/releases/v0.2.0"}})";
    std::string version, title, url;
    CHECK(updater::ParseRelease(json, version, title, url));
    CHECK_EQ(version, std::string("0.2.0"));
    CHECK_EQ(title, std::string("TCSCC Recomp 0.2.0"));
    CHECK_EQ(url, std::string("https://gitlab.com/g/p/-/releases/v0.2.0/downloads/TCSCC_Setup.exe"));

    // No setup exe among the links: the version still reads, no installer.
    const std::string noExe = R"({"name":"r","tag_name":"0.3.0","assets":{"links":[{"name":"notes.txt","url":"https://x/notes.txt"}]}})";
    CHECK(updater::ParseRelease(noExe, version, title, url));
    CHECK_EQ(version, std::string("0.3.0"));
    CHECK_EQ(url, std::string());

    // Another .exe name is taken when TCSCC_Setup.exe is missing.
    const std::string other = R"({"tag_name":"v1.0","assets":{"links":[{"name":"Setup 1.0","url":"https://x/u/setup.exe"}]}})";
    CHECK(updater::ParseRelease(other, version, title, url));
    CHECK_EQ(url, std::string("https://x/u/setup.exe"));

    CHECK(!updater::ParseRelease(R"({"message":"404 Not Found"})", version, title, url));
}

TEST(updater_parse_forge_release)
{
    // Shape of git.8gramlabs.com/api/v1/repos/<owner>/<repo>/releases/latest (Forgejo), trimmed.
    const std::string json = R"({"id":9,"tag_name":"v0.1.6","target_commitish":"main","name":"TCSCC Recomp 0.1.6","body":"notes",)"
        R"("draft":false,"prerelease":false,"author":{"id":1,"login":"siteadmin"},"assets":[)"
        R"({"id":4,"name":"TCSCC_Setup_0.1.6.zip","size":10,"download_count":1,"uuid":"a","browser_download_url":"https://git.8gramlabs.com/8gramlabs/TCSCC-Recomp/releases/download/v0.1.6/TCSCC_Setup_0.1.6.zip","type":"attachment"},)"
        R"({"id":5,"name":"TCSCC_Setup.exe","size":20,"download_count":2,"uuid":"b","browser_download_url":"https://git.8gramlabs.com/8gramlabs/TCSCC-Recomp/releases/download/v0.1.6/TCSCC_Setup.exe","type":"attachment"}]})";
    std::string version, title, url;
    CHECK(updater::ParseRelease(json, version, title, url));
    CHECK_EQ(version, std::string("0.1.6"));
    CHECK_EQ(title, std::string("TCSCC Recomp 0.1.6"));
    CHECK_EQ(url, std::string("https://git.8gramlabs.com/8gramlabs/TCSCC-Recomp/releases/download/v0.1.6/TCSCC_Setup.exe"));
}
