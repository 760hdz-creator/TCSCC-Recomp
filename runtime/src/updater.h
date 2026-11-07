// In-game updates (update notification at the main menu, F1 menu > Update tab): the latest release
// of the port's GitLab project (SCDA_UPDATE_REPO, group/project; [Update] Repo= in settings.ini
// overrides it), compared with this build's version (SCDA_VERSION, the CMake project version).
// Only a strictly higher version is offered, never a downgrade. The release's TCSCC_Setup.exe link
// is downloaded to %LOCALAPPDATA%\TCSCCRecomp\update (checked: starts with "MZ", full length), then
// the game quits and a hidden watcher runs that installer's --update over this install folder
// (elevated only when the folder is not writable) and starts the game again.
// Adapted from the TTT2 Recomp updater (same author).
#pragma once
#include <string>

namespace updater {

enum class State { Idle, Checking, UpToDate, Available, NoReleases, Failed, Downloading, Downloaded };

struct Status
{
    State state = State::Idle;
    std::string latest;     // the release's version ("0.2.0", tag without a leading v), once checked
    std::string title;      // the release's name
    std::string error;      // Failed: why
    bool download = false;  // Failed: in the download (else in the check)
    int percent = 0;        // Downloading
};

const char* CurrentVersion(); // SCDA_VERSION, or SCDA_VERSION_OVERRIDE (tests: pretend to be an older build)
const std::string& Repo();
// https://gitlab.com/<repo>/-/releases
std::string ReleasePageUrl();
Status Get();
void Check();    // in the background; ignored while busy
void Download(); // in the background, after a check found a newer version; after a failed check, checks again
void OpenReleasePage();
// True when this exe runs from an installed copy (game\default.img next to it): an update can be applied.
bool CanInstall();
// Downloaded: starts the watcher that waits for the game to close, runs the installer's --update
// and starts the game again. On true the caller quits the game at once.
bool Install(std::string& error);

// Pure helpers (tests).
// <0, 0, >0 like strcmp; numeric fields ("0.10.0" > "0.9.1"), a leading v ignored, missing fields 0.
int CompareVersions(const std::string& a, const std::string& b);
// The string value of "key" in a JSON object (first occurrence, any depth), unescaped; empty if absent.
std::string JsonString(const std::string& json, const std::string& key);
// A GitLab release object (GET /projects/:id/releases/permalink/latest): its version (tag_name
// without a leading v) and the installer's URL (the assets.links entry named TCSCC_Setup.exe, else
// the first .exe link; direct_asset_url preferred). False when the JSON holds no tag.
bool ParseRelease(const std::string& json, std::string& version, std::string& title, std::string& installerUrl);

} // namespace updater
