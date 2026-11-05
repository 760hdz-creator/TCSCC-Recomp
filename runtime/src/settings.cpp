#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <vector>
#include <string>
#include <settings.h>
#include <runtime.h>

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

// Calls f(key, value) for every key=value line of [section] (all lowercased).
template <typename F>
static void ForEachSetting(const std::filesystem::path& ini, const char* wanted, F f)
{
    std::ifstream in(ini);
    std::string line, section;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 2 && line.front() == '[')
        {
            section = Lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (section != wanted || eq == std::string::npos)
            continue;
        f(Lower(line.substr(0, eq)), Lower(line.substr(eq + 1)));
    }
}

static bool Truthy(const std::string& value) { return value != "0" && value != "false"; }

InputSettings LoadInputSettings(const std::filesystem::path& ini)
{
    InputSettings s;
    ForEachSetting(ini, "input", [&](const std::string& key, const std::string& value)
    {
        if (key == "mouselook")
            s.mouseLook = Truthy(value);
        else if (key == "mouseinverty")
            s.mouseInvertY = Truthy(value);
        else if (key == "playstationnative")
            s.sdlGamepads = Truthy(value);
        else if (key == "buttonprompts")
            s.buttonPrompts = value;
        else if (key == "mousesensitivity")
        {
            float v = float(atof(value.c_str()));
            if (v >= 0.1f && v <= 10.0f)
                s.mouseSensitivity = v;
        }
    });
    return s;
}

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini)
{
    DisplaySettings s;
    ForEachSetting(ini, "display", [&](const std::string& key, const std::string& value)
    {
        if (key == "scale")
        {
            int v = atoi(value.c_str());
            if (v >= 1 && v <= 4)
                s.scale = uint32_t(v);
        }
        else if (key == "mode")
        {
            if (value == "windowed") s.mode = DisplaySettings::Windowed;
            else if (value == "borderless") s.mode = DisplaySettings::Borderless;
            else if (value == "fullscreen") s.mode = DisplaySettings::Fullscreen;
        }
        else if (key == "vsync")
            s.vsync = Truthy(value);
        else if (key == "texturesharpness")
        {
            int v = atoi(value.c_str());
            if (v >= 0 && v <= 2)
                s.textureSharpness = uint32_t(v);
        }
        else if (key == "anisotropy")
        {
            int v = atoi(value.c_str());
            if (v >= 1 && v <= 16)
                s.anisotropy = uint32_t(v);
        }
        else if (key == "cleangoggles")
            s.cleanGoggles = Truthy(value);
        else if (key == "ultrawide")
            s.ultrawide = Truthy(value);
        else if (key == "fps60")
        {
            if (Truthy(value) && s.frameRate == 30)
                s.frameRate = 60;
        }
        else if (key == "framerate")
        {
            const int v = atoi(value.c_str());
            for (uint32_t r : kFrameRates)
                if (int(r) == v)
                    s.frameRate = r;
        }
        else if (key == "drawdistance")
        {
            for (uint32_t l = 0; l < 4; l++)
                if (value == Lower(DrawDistanceName(l)))
                    s.drawDistance = l;
        }
        else if (key == "drawbackfaces")
            s.drawBackFaces = Truthy(value);
        else if (key == "fov")
        {
            int v = atoi(value.c_str());
            if (v >= 70 && v <= 120)
                s.fov = uint32_t(v);
        }
        else if (key == "antialiasing")
        {
            if (value == "off") s.antiAliasing = DisplaySettings::AaOff;
            else if (value == "fxaa") s.antiAliasing = DisplaySettings::AaFxaa;
        }
        else if (key == "sharpening")
        {
            int v = atoi(value.c_str());
            if (v >= 0 && v <= 100)
                s.sharpening = uint32_t(v);
        }
        else if (key == "gamma")
        {
            int v = atoi(value.c_str());
            if (v >= 50 && v <= 200)
                s.gamma = uint32_t(v);
        }
        else if (key == "upscaling")
        {
            for (int m = DisplaySettings::UpOff; m <= DisplaySettings::UpUltraPerformance; m++)
                if (value == Lower(UpscalingName(DisplaySettings::Upscaling(m))))
                    s.upscaling = DisplaySettings::Upscaling(m);
        }
    });
    return s;
}

const char* UpscalingName(DisplaySettings::Upscaling mode)
{
    static const char* const names[] = { "Off", "Native", "Quality", "Balanced", "Performance", "UltraPerformance" };
    return names[std::clamp(int(mode), 0, 5)];
}

static std::atomic<int> g_drawDistance{0};
void SetDrawDistance(int level) { g_drawDistance = level; }
int DrawDistance() { return g_drawDistance; }

const char* DrawDistanceName(uint32_t level)
{
    static const char* const names[] = { "Console", "High", "Ultra", "Maximum" };
    return names[std::min(level, 3u)];
}

const char* AntiAliasingName(DisplaySettings::AntiAliasing aa) { return aa == DisplaySettings::AaFxaa ? "FXAA" : "Off"; }

uint32_t UpscalingScale(DisplaySettings::Upscaling mode, uint32_t windowHeight, uint32_t manualScale)
{
    static const double ratios[] = { 1.0, 1.0, 1.5, 1.7, 2.0, 3.0 };
    if (mode == DisplaySettings::UpOff || !windowHeight)
        return std::clamp<uint32_t>(manualScale, 1, 4);
    const double rows = double(windowHeight) / ratios[std::clamp(int(mode), 0, 5)];
    return std::clamp<uint32_t>(uint32_t(std::lround(rows / 720.0)), 1, 4);
}

UpdateSettings LoadUpdateSettings(const std::filesystem::path& ini)
{
    UpdateSettings s;
    ForEachSetting(ini, "update", [&](const std::string& key, const std::string& value)
    {
        if (key == "checkonstart")
            s.checkOnStart = Truthy(value);
        else if (key == "repo" && value.find('/') != std::string::npos)
            s.repo = value;
    });
    return s;
}

// X_LANGUAGE ids the Conviction US/EU disc carries text + voices for (Default\loc-int/fra/deu/esp/ita.umd,
// Sounds\Xbox360\<Name>). loc-dan/nl/nor/swe.umd also exist but have no X_LANGUAGE id here.
static const struct { uint32_t id; const char* name; } kLanguages[] = {
    { 1, "English" }, { 3, "German" }, { 4, "French" }, { 5, "Spanish" }, { 6, "Italian" },
};

const char* GameLanguageName(uint32_t id)
{
    for (auto& l : kLanguages)
        if (l.id == id)
            return l.name;
    return nullptr;
}

static uint32_t ParseLanguage(std::string value)
{
    for (auto& c : value)
        c = char(tolower(uint8_t(c)));
    for (auto& l : kLanguages)
    {
        std::string n = l.name;
        for (auto& c : n)
            c = char(tolower(uint8_t(c)));
        if (value == n)
            return l.id;
    }
    const uint32_t n = uint32_t(strtoul(value.c_str(), nullptr, 10));
    return GameLanguageName(n) ? n : 0;
}

uint32_t GameLanguage()
{
    static const uint32_t id = []
    {
        uint32_t v = 0;
        if (const char* env = getenv("SCDA_LANGUAGE"))
            v = ParseLanguage(env);
        else
            ForEachSetting(SettingsPath(), "general", [&](const std::string& key, const std::string& value)
            {
                if (key == "language")
                    v = ParseLanguage(value);
            });
        if (v > 1)
        {
            // The disc must carry the text (Conviction: Default\loc-<code>.umd); else English, as a console would.
            static const char* const codes[] = { "", "int", "", "deu", "fra", "esp", "ita" };
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            const auto dir = std::filesystem::path(exe).parent_path() / "game" / "Default" / (std::string("loc-") + codes[v] + ".umd");
            std::error_code ec;
            if (!std::filesystem::is_regular_file(dir, ec))
            {
                RuntimeLog("[settings] Language %s: %s missing on this disc, using English\n", GameLanguageName(v), dir.string().c_str());
                v = 1;
            }
        }
        return v ? v : 1u;
    }();
    return id;
}

bool SaveSetting(const std::filesystem::path& ini, const std::string& section, const std::string& key, const std::string& value)
{
    std::vector<std::string> lines;
    {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }
    const std::string wantSection = Lower(section), wantKey = Lower(key);
    std::string current;
    size_t sectionEnd = std::string::npos; // index after the wanted section's last line
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; i++)
    {
        const std::string& line = lines[i];
        if (line.size() > 2 && line.front() == '[')
        {
            current = Lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        if (current != wantSection)
            continue;
        sectionEnd = i + 1;
        const size_t eq = line.find('=');
        if (eq != std::string::npos && Lower(line.substr(0, eq)) == wantKey)
        {
            lines[i] = key + "=" + value;
            done = true;
        }
    }
    if (!done)
    {
        if (sectionEnd == std::string::npos)
        {
            // Section header without keys: insert right after it; no section: append one.
            for (size_t i = 0; i < lines.size(); i++)
                if (lines[i].size() > 2 && lines[i].front() == '[' && Lower(lines[i].substr(1, lines[i].find(']') - 1)) == wantSection)
                    sectionEnd = i + 1;
        }
        if (sectionEnd == std::string::npos)
        {
            if (!lines.empty() && !lines.back().empty())
                lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(key + "=" + value);
        }
        else
        {
            // After the section's last key, before blank lines that separate it from the next.
            while (sectionEnd > 0 && lines[sectionEnd - 1].empty())
                sectionEnd--;
            lines.insert(lines.begin() + sectionEnd, key + "=" + value);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(ini.parent_path(), ec);
    std::filesystem::path tmp = ini;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const std::string& line : lines)
            out << line << "\r\n";
        if (!out)
            return false;
    }
    return MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool OnlineEnabledSetting()
{
    bool on = false;
    ForEachSetting(SettingsPath(), "online", [&](const std::string& key, const std::string& value) {
        if (key == "enabled")
            on = Truthy(value);
    });
    return on;
}

std::filesystem::path SettingsPath()
{
    // SCDA_SETTINGS=<file>: another settings.ini (unattended test runs never touch the user's).
    if (const char* file = getenv("SCDA_SETTINGS"); file && *file)
        return file;
    PWSTR local = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        p = std::filesystem::path(local) / "TCSCCRecomp" / "settings.ini";
    CoTaskMemFree(local);
    return p;
}
