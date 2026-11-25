// GameVault for PS5 - The native app: Library, game page, Downloads, Storage, Settings.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app is a window onto two services:
//   - GameVault for PS5, the background payload on this console
//     (http://127.0.0.1:8687): settings, drives, games on the PS5, downloads.
//     It keeps downloading while this app is closed.
//   - The GameVault server on the NAS: the library, covers and details.
// If the background service isn't running, the app starts it by sending the
// copy of gamevault.elf kept in its own folder to the ELF loader.

#include "concepts/concepts.hpp"
#include "concepts/gv_image.hpp"
#include "concepts/gv_json.hpp"
#include "concepts/gv_net.hpp"

#include "core/tween.hpp"
#include "ui/components.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Align;
using gfx::Color;
using gfx::Rect;

// ---- The design language (the same as the web interface) --------------------------
const Color kBg = Color::rgb(0x0A0C10);
const Color kPanel = Color::rgb(0x11151C);
const Color kLine = Color::rgb(0x1F2630);
const Color kInk = Color::rgb(0xE8ECF1);
const Color kInk2 = Color::rgb(0xC9D2DE);
const Color kMuted = Color::rgb(0x9AA4B2);
const Color kAccent = Color::rgb(0x5B8CFF);
const Color kAccent2 = Color::rgb(0xA9C2FF);
const Color kGood = Color::rgb(0x3FCF8E);
const Color kWarn = Color::rgb(0xF5B841);
const Color kBad = Color::rgb(0xF07A7A);
const Color kWhite = Color::rgb(0xFFFFFF);

constexpr const char *kService = "http://127.0.0.1:8687";
constexpr float kMargin = 64.0f;
constexpr float kSideX = 1920.0f - kMargin + 40.0f; // content right edge + 40 (no side panel any more)

constexpr const char *kTechniques[] = {
    "Library from your GameVault server, with covers",
    "Downloads run in the background service, also while this app is closed",
    "Download, update, space and firmware checks per game",
};

const ui::Theme &acrylic()
{
    for (const ui::Theme &theme : ui::themes())
        if (std::strcmp(theme.id, "acrylic") == 0)
            return theme;
    return ui::themes()[0];
}

std::string format_bytes(double n)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1000.0 && i < 4)
    {
        n /= 1000.0;
        ++i;
    }
    char buf[32];
    if (i == 0 || n >= 100.0)
        std::snprintf(buf, sizeof buf, "%.0f %s", n, units[i]);
    else
        std::snprintf(buf, sizeof buf, "%.1f %s", n, units[i]);
    return buf;
}

std::string format_eta(double remaining, double bps)
{
    if (bps <= 0 || remaining <= 0)
        return "";
    const long s = static_cast<long>(remaining / bps);
    char buf[48];
    if (s < 60)
        std::snprintf(buf, sizeof buf, "%ld s left", s);
    else if (s < 3600)
        std::snprintf(buf, sizeof buf, "%ld min left", (s + 30) / 60);
    else
        std::snprintf(buf, sizeof buf, "%ld h %ld min left", s / 3600, (s % 3600) / 60);
    return buf;
}

int compare_versions(const std::string &a, const std::string &b)
{
    std::size_t i = 0, j = 0;
    for (;;)
    {
        while (i < a.size() && (a[i] < '0' || a[i] > '9'))
            ++i;
        while (j < b.size() && (b[j] < '0' || b[j] > '9'))
            ++j;
        if (i >= a.size() && j >= b.size())
            return 0;
        long x = 0, y = 0;
        while (i < a.size() && a[i] >= '0' && a[i] <= '9')
            x = x * 10 + (a[i++] - '0');
        while (j < b.size() && b[j] >= '0' && b[j] <= '9')
            y = y * 10 + (b[j++] - '0');
        if (x != y)
            return x < y ? -1 : 1;
    }
}

// The kit's fonts have no ™, ® or ©: drop them from names.
std::string clean(std::string s)
{
    for (const char *mark : {"\u2122", "\u00AE", "\u00A9"})
    {
        std::size_t at;
        while ((at = s.find(mark)) != std::string::npos)
            s.erase(at, std::strlen(mark));
    }
    return s;
}

std::string upper_ascii(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// Shortens text with an ellipsis until it fits `width`.
// Fast start, gentle stop (k from 0 to 1).
float ease_out(float k)
{
    k = std::max(0.0f, std::min(1.0f, k));
    const float r = 1.0f - k;
    return 1.0f - r * r * r;
}

// The part of a w x h picture that fills `box` without stretching it: the
// picture is scaled to cover the box and the overflow is cropped, around
// (fx, fy) (0.5, 0.5 is the middle; a lower fy keeps more of the top).
Rect fill_uv(int w, int h, const Rect &box, float fx = 0.5f, float fy = 0.5f)
{
    if (w <= 0 || h <= 0 || box.w <= 0 || box.h <= 0)
        return gfx::kFullUv;
    const float pic = static_cast<float>(w) / static_cast<float>(h);
    const float want = box.w / box.h;
    if (pic > want) // wider than the box: crop the sides
    {
        const float u = want / pic;
        return {(1.0f - u) * fx, 0.0f, u, 1.0f};
    }
    const float v = pic / want; // taller: crop top and bottom
    return {0.0f, (1.0f - v) * fy, 1.0f, v};
}

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    if (font.measure(text, size) <= width)
        return text;
    std::string s = text;
    while (!s.empty())
    {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
            s.pop_back(); // whole UTF-8 characters
        if (font.measure(s + "...", size) <= width)
            return s + "...";
    }
    return "...";
}

// ---- Data -----------------------------------------------------------------------------

struct Game
{
    std::int64_t id = 0;
    std::string title, platform, title_id, version, cover_url, added, release, required_fw;
    std::int64_t size = 0;
    bool favourite = false;
    std::uint32_t cover = 0; // texture, 0 until loaded
    int cover_w = 0, cover_h = 0;
    bool cover_asked = false;
    bool cover_failed = false;
};

struct Drive
{
    std::string id, label;
    double free = 0, total = 0;
};

struct Installed
{
    std::string title_id, version, title, path, drive, kind;
    double size = 0;
};

struct Job
{
    std::string id, title, platform, state, error, note, drive, version;
    std::int64_t game_id = 0;
    double total = 0, done = 0, speed = 0;
};

struct ServiceState
{
    bool loaded = false;
    std::string version, firmware;
    bool game_running = false;
    // settings
    std::string server, ps5_drive, transfer;
    int connections = 0, measured_connections = 0, measured_mbps = 0, play_limit = 0;
    bool notify = true, check_files = true;
    std::vector<Drive> drives;
    std::map<std::string, double> reserved;
    std::vector<Installed> installed;
    std::vector<Job> jobs;
    // tile / updates
    bool update_ready = false;
    std::string elf_version, latest_version;
};

ServiceState parse_state(const gv::Json &j)
{
    ServiceState s;
    s.loaded = true;
    s.version = j["version"].str();
    s.firmware = j["firmware"].str();
    s.game_running = j["gameRunning"].boolean();
    const gv::Json &set = j["settings"];
    s.server = set["server"].str();
    s.ps5_drive = set["ps5Drive"].str("internal");
    s.transfer = set["transfer"].str("auto");
    s.connections = static_cast<int>(set["connections"].i64());
    s.measured_connections = static_cast<int>(set["measuredConnections"].i64());
    s.measured_mbps = static_cast<int>(set["measuredMBps"].i64());
    s.play_limit = static_cast<int>(set["playLimitMBps"].i64());
    s.notify = set["notify"].boolean(true);
    s.check_files = set["checkFiles"].boolean(true);
    for (const gv::Json &d : j["drives"].items())
        s.drives.push_back({d["id"].str(), d["label"].str(), d["free"].num(), d["total"].num()});
    const gv::Json &reserved = j["reserved"];
    for (std::size_t i = 0; i < reserved.keys().size(); ++i)
        s.reserved[reserved.keys()[i]] = reserved.items()[i].num();
    for (const gv::Json &it : j["installed"].items())
        s.installed.push_back({upper_ascii(it["titleId"].str()), it["version"].str(), clean(it["title"].str()),
                               it["path"].str(), it["drive"].str(), it["kind"].str(), it["size"].num()});
    for (const gv::Json &d : j["downloads"].items())
    {
        Job job;
        job.id = d["id"].str();
        job.game_id = d["gameId"].i64();
        job.title = clean(d["title"].str());
        job.platform = d["platform"].str();
        job.state = d["state"].str();
        job.error = d["error"].str();
        job.note = d["note"].str();
        job.drive = d["drive"].str();
        job.version = d["version"].str();
        job.total = d["total"].num();
        job.done = d["done"].num();
        job.speed = d["speed"].num();
        s.jobs.push_back(job);
    }
    const gv::Json &tile = j["tile"];
    s.update_ready = tile["updateReady"].boolean();
    s.elf_version = tile["elfVersion"].str();
    s.latest_version = tile["latestVersion"].str();
    return s;
}

bool active_state(const std::string &state)
{
    return state == "queued" || state == "downloading" || state == "verifying" || state == "paused";
}

enum class Status
{
    download,
    downloaded,
    update,
    queued,
    downloading,
    paused,
    failed,
};

struct GameStatus
{
    Status status = Status::download;
    const Installed *installed = nullptr;
    const Job *job = nullptr;
};

GameStatus status_of(const Game &g, const ServiceState &s)
{
    GameStatus out;
    const Job *failed = nullptr;
    for (const Job &j : s.jobs)
    {
        if (j.game_id != g.id)
            continue;
        if (active_state(j.state))
            out.job = &j;
        else if (j.state == "failed")
            failed = &j;
    }
    const std::string tid = upper_ascii(g.title_id);
    for (const Installed &i : s.installed)
        if (!tid.empty() && i.title_id == tid &&
            (!out.installed || compare_versions(i.version, out.installed->version) > 0))
            out.installed = &i;
    if (out.job)
    {
        out.status = out.job->state == "paused" ? Status::paused
                     : out.job->state == "queued" ? Status::queued
                                                  : Status::downloading;
        return out;
    }
    if (out.installed)
    {
        const bool newer = !g.version.empty() && !out.installed->version.empty() &&
                           compare_versions(g.version, out.installed->version) > 0;
        out.status = newer ? Status::update : Status::downloaded;
        return out;
    }
    if (failed)
    {
        out.status = Status::failed;
        out.job = failed;
    }
    return out;
}

const char *drive_name(const std::string &id)
{
    static std::string name;
    if (id == "internal")
        return "Internal SSD";
    if (id.rfind("usb", 0) == 0)
        name = "USB drive " + std::to_string(id[3] - '0' + 1);
    else if (id.rfind("ext", 0) == 0)
        name = "M.2 storage " + std::to_string(id[3] - '0' + 1);
    else
        name = id;
    return name.c_str();
}

int fw_compare(const std::string &a, const std::string &b)
{
    return compare_versions(a, b);
}

// ---- The screen -----------------------------------------------------------------------

enum class Section
{
    library,
    downloads,
    storage,
    settings,
};
constexpr int kSections = 4;
constexpr const char *kSectionNames[kSections] = {"Library", "Downloads", "Storage", "Settings"};

constexpr const char *kFilters[] = {"All", "PS5", "PS4", "On this PS5", "Not on this PS5", "Updates", "Favourites"};
constexpr int kFilterCount = 7;
constexpr const char *kSorts[] = {"Title A–Z", "Date added", "Release date", "Size"};
constexpr int kSortCount = 4;

// The grid
constexpr int kColumns = 7;
constexpr float kTileW = 228.0f;
constexpr float kTileH = 342.0f;
constexpr float kTileGapX = 32.0f; // 7 x 228 + 6 x 32 = 1788: the full width
constexpr float kRowH = kTileH + 84.0f;
constexpr float kGridTop = 228.0f;

enum class SettingRow
{
    server,
    test,
    drive,
    connections,
    speed_test,
    play_limit,
    notify,
    check_files,
    update,
    stop,
};

class GameVault final : public app::Concept
{
  public:
    explicit GameVault(app::Context &context)
        : context_(context), api_(2), images_(2)
    {
        prompt_.style.theme = acrylic();
        prompt_.style.allow_empty = true;
        prompt_.style.auto_capital = false;
        prompt_.style.max_length = 120;
        prompt_.keyboard.style.bindings = ui::KeyboardBindings::standard();
        prompt_.keyboard.set_layouts({ui::KeyboardLayout::letters(), ui::KeyboardLayout::symbols()});
        snapshot_ = std::getenv("GV_SNAPSHOT") != nullptr;
        sys::log("[GV] GameVault app starting");
        status_line_ = "Connecting to GameVault on this PS5...";
        if (snapshot_)
            load_everything_now();
        else
            refresh_state();
    }

    ~GameVault() override
    {
        for (Game &g : games_)
            gv::delete_texture(g.cover);
        clear_detail_media();
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "gamevault",
            "GameVault",
            "Your NAS's PS5 and PS4 games, downloaded to this console",
            "app/screens/gamevault.cpp",
            audio::SoundSet::glass,
            kAccent,
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    // ---- opening animation ---------------------------------------------------------------

    // Shown from launch until the library is ready (or there's something to
    // say about why it isn't), and for at least 1.4 s so it reads as intended.
    bool splash_ready() const
    {
        if (snapshot_)
            return splash_age_ > 1.4f; // long enough for the tour to picture it
        if (splash_age_ > 14.0f)
            return true; // the screens explain what's wrong from here
        if (splash_age_ < 1.4f)
            return false;
        if (!service_ok_)
            return false;
        return server().empty() || !games_.empty() || !games_error_.empty();
    }

    void draw_splash(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        const float fade = splash_out_ > 0.0f ? std::max(0.0f, 1.0f - splash_out_ / 0.45f) : 1.0f;
        list.push_opacity(fade);
        list.gradient_rect({0, 0, 1920, 1080}, 0, kBg, Color::rgb(0x111A2E));
        const float t = splash_age_;
        // The V: each stroke grows from its top, the second a moment later.
        const float s = 3.2f, mx = 960 - 32 * s, my = 330;
        const float a = ease_out(std::min(1.0f, t / 0.55f));
        const float b = ease_out(std::max(0.0f, std::min(1.0f, (t - 0.18f) / 0.55f)));
        auto lerp = [](float p, float q, float k) { return p + (q - p) * k; };
        if (a > 0)
        {
            const float left[] = {mx + 7 * s, my + 7 * s, mx + 20 * s, my + 7 * s,
                                  mx + lerp(20, 37, a) * s, my + lerp(7, 57, a) * s, mx + lerp(7, 25, a) * s, my + lerp(7, 57, a) * s};
            list.polygon(left, 4, kAccent);
        }
        if (b > 0)
        {
            const float right[] = {mx + 57 * s, my + 7 * s, mx + 44 * s, my + 7 * s,
                                   mx + lerp(44, 27, b) * s, my + lerp(7, 57, b) * s, mx + lerp(57, 39, b) * s, my + lerp(7, 57, b) * s};
            list.polygon(right, 4, kAccent2);
        }
        // a soft glow that breathes while it waits
        const float glow = 0.18f + 0.08f * std::sin(t * 2.6f);
        list.glow({960 - 110, my + 10, 220, 170}, 60, 70, kAccent.with_alpha(glow * b));
        // The name
        const float n = ease_out(std::max(0.0f, std::min(1.0f, (t - 0.45f) / 0.5f)));
        list.push_opacity(n);
        const float w1 = f.semibold.measure("Game", 64), w2 = f.semibold.measure("Vault", 64);
        const float nx = 960 - (w1 + w2) / 2;
        const float ny = 640 + (1.0f - n) * 18;
        ui::text(list, f.semibold, "Game", nx, ny, 64, kInk);
        ui::text(list, f.semibold, "Vault", nx + w1, ny, 64, kAccent);
        list.pop_opacity();
        // Three dots, then what it's doing
        const float d = std::max(0.0f, std::min(1.0f, (t - 0.9f) / 0.4f));
        list.push_opacity(d);
        for (int i = 0; i < 3; ++i)
        {
            const float k = 0.5f + 0.5f * std::sin(t * 5.0f - static_cast<float>(i) * 0.9f);
            list.circle(960 - 28 + static_cast<float>(i) * 28, 720, 6 + 2 * k, kAccent2.with_alpha(0.35f + 0.65f * k));
        }
        std::string line;
        if (!service_ok_)
            line = status_line_.empty() ? "Starting GameVault" : status_line_;
        else if (!server().empty() && games_.empty())
            line = "Loading your library";
        ui::text(list, f.regular, fit(f.regular, line, 22, 1400), 960, 790, 22, kMuted, Align::center);
        list.pop_opacity();
        list.pop_opacity();
    }

    // ---- update ---------------------------------------------------------------------

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        splash_age_ += dt;
        if (splash_on_ && splash_ready())
        {
            splash_out_ += dt;
            if (splash_out_ > 0.45f)
            {
                splash_on_ = false;
                age_ = 0.0f; // the library's tiles fade in after the splash
            }
        }
        api_.run_posted();
        images_.run_posted();
        poll(dt);
        toast_time_ = std::max(0.0f, toast_time_ - dt);
        if (splash_on_ && splash_out_ == 0.0f)
            return;

        if (prompt_.is_open())
        {
            const ui::Event event = prompt_.handle(input, feedback);
            if (event == ui::Event::activated)
                save_setting("server", gv::json_quote(prompt_.text()));
            prompt_.update(dt);
            return;
        }
        prompt_.update(dt);

        if (confirm_open_)
        {
            if (input.is_pressed(Action::confirm))
            {
                confirm_open_ = false;
                feedback.play(audio::Cue::select);
                if (confirm_action_)
                    confirm_action_();
            }
            else if (input.is_pressed(Action::back))
            {
                confirm_open_ = false;
                feedback.play(audio::Cue::back);
            }
            return;
        }

        if (!details_open_ && (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev)))
        {
            const int step = input.is_pressed(Action::page_next) ? 1 : kSections - 1;
            section_ = static_cast<Section>((static_cast<int>(section_) + step) % kSections);
            feedback.play(audio::Cue::tab);
            section_age_ = 0.0f;
        }
        section_age_ += dt;

        if (details_open_)
            update_details(input, feedback);
        else
        {
            switch (section_)
            {
            case Section::library:
                update_library(input, feedback, dt);
                break;
            case Section::downloads:
                update_downloads(input, feedback);
                break;
            case Section::storage:
                update_storage(input, feedback);
                break;
            case Section::settings:
                update_settings(input, feedback);
                break;
            }
        }
        scroll_.update(dt, 14.0f);
        list_scroll_.update(dt, 14.0f);
        page_scroll_.update(dt, 12.0f);
        media_scroll_.update(dt, 14.0f);
        request_visible_covers();
    }

    // ---- draw ------------------------------------------------------------------------

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = kBg;
        frame.backdrop.colors[1] = Color::rgb(0x111A2E);
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        if (details_open_)
        {
            draw_details(list); // the key art goes under the header, not over it
            draw_header(list);
        }
        else
        {
            draw_header(list);
            switch (section_)
            {
            case Section::library:
                draw_library(list);
                break;
            case Section::downloads:
                draw_downloads(list);
                break;
            case Section::storage:
                draw_storage(list);
                break;
            case Section::settings:
                draw_settings(list);
                break;
            }
        }
        draw_hints(list);
        if (!details_open_ && state_.loaded)
            draw_status_strip(list);
        if (toast_time_ > 0.0f)
            draw_toast(frame.overlay);
        if (details_open_ && viewer_open_)
            draw_viewer(frame.overlay);
        if (splash_on_)
            draw_splash(frame.overlay);
        if (confirm_open_)
            draw_confirm(frame.overlay);
        if (prompt_.is_open() || prompt_.visible())
        {
            frame.glass = true;
            ui::Canvas canvas{frame.overlay, context_.fonts, frame.glass_texture, clock_};
            prompt_.draw(canvas);
        }
    }

    std::span<const app::TourStep> tour() const override
    {
        static const app::TourStep kTour[] = {
            {1.1f, 0, Direction::none, "opening"},
            {1.5f, 0, Direction::none, "library"},
            {0.4f, 0, Direction::right},
            {0.6f, action_bit(Action::confirm), Direction::none, nullptr},
            {2.0f, 0, Direction::none, "details"},
            {0.4f, 0, Direction::down},
            {1.2f, 0, Direction::none, "details_media"},
            {0.4f, 0, Direction::right},
            {0.4f, action_bit(Action::confirm), Direction::none, nullptr},
            {0.8f, 0, Direction::none, "screenshot"},
            {0.4f, action_bit(Action::back), Direction::none, nullptr},
            {0.4f, 0, Direction::down},
            {1.2f, 0, Direction::none, "details_info"},
            {0.4f, action_bit(Action::back), Direction::none, nullptr},
            {0.6f, action_bit(Action::page_next), Direction::none, nullptr},
            {0.8f, 0, Direction::none, "downloads"},
            {0.4f, action_bit(Action::page_next), Direction::none, nullptr},
            {0.8f, 0, Direction::none, "storage"},
            {0.4f, action_bit(Action::page_next), Direction::none, nullptr},
            {0.8f, 0, Direction::none, "settings"},
        };
        return kTour;
    }

  private:
    // ---- service and server ------------------------------------------------------------

    std::string server() const
    {
        std::string s = state_.server;
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        return s;
    }

    std::string media(const std::string &url) const
    {
        if (url.empty())
            return url;
        if (url.rfind("http://", 0) == 0)
            return url;
        if (url.rfind("https://", 0) == 0)
            return ""; // the app has no TLS: covers come from GameVault's own copy
        return server() + url;
    }

    void toast(const std::string &text, bool error = false)
    {
        toast_ = text;
        toast_error_ = error;
        toast_time_ = 4.0f;
    }

    void poll(float dt)
    {
        if (snapshot_)
            return;
        since_state_ += dt;
        since_games_ += dt;
        if (!state_busy_ && since_state_ > (state_.loaded ? 1.5f : 3.0f))
            refresh_state();
        if (state_.loaded && !server().empty() && !games_busy_ &&
            (since_games_ > 60.0f || games_server_ != server()))
            refresh_games();
    }

    void apply_state(const gv::HttpResult &r)
    {
        if (r.ok())
        {
            bool ok = false;
            const gv::Json j = gv::Json::parse(r.body, &ok);
            if (ok)
            {
                state_ = parse_state(j);
                service_ok_ = true;
                status_line_.clear();
                return;
            }
        }
        service_ok_ = false;
    }

    void refresh_state()
    {
        state_busy_ = true;
        since_state_ = 0.0f;
        const bool try_start = !service_ok_ && !start_tried_;
        if (try_start)
            start_tried_ = true;
        api_.run([this, try_start] {
            gv::HttpResult r = gv::http("GET", std::string(kService) + "/api/state", {}, 5);
            std::string note;
            if (!r.connected && try_start)
            {
                // The background service isn't running: start it from our own folder.
                std::string error;
                if (access("/app0/gamevault.elf", R_OK) == 0 &&
                    (gv::send_file_to_local_port("/app0/gamevault.elf", 9021, &error) ||
                     gv::send_file_to_local_port("/app0/gamevault.elf", 9027, &error)))
                {
                    for (int i = 0; i < 40 && !r.connected; ++i)
                    {
                        usleep(250000);
                        r = gv::http("GET", std::string(kService) + "/api/state", {}, 5);
                    }
                    note = r.connected ? "" : "Started GameVault, but it isn't answering yet";
                }
                else
                    note = access("/app0/gamevault.elf", R_OK) == 0
                               ? "Couldn't start GameVault: " + error
                               : "GameVault isn't running. Load gamevault.elf with your payload loader.";
            }
            if (!note.empty() || !r.connected)
                sys::log("[GV] service: %s %s", r.connected ? "answering" : "not answering", note.c_str());
            api_.post([this, r, note] {
                state_busy_ = false;
                const bool was = service_ok_;
                apply_state(r);
                if (service_ok_ && !was)
                    sys::log("[GV] connected to the service (%s)", state_.version.c_str());
                if (!service_ok_)
                    status_line_ = !note.empty() ? note
                                                 : "GameVault isn't running on this PS5. Load gamevault.elf with your payload loader.";
            });
        });
    }

    void refresh_games()
    {
        games_busy_ = true;
        since_games_ = 0.0f;
        const std::string base = server();
        api_.run([this, base] {
            gv::HttpResult r = gv::http("GET", base + "/api/v1/games?apps=hide&sort=title", {}, 15);
            api_.post([this, r, base] {
                games_busy_ = false;
                games_server_ = base;
                if (!r.ok())
                {
                    games_error_ = r.error.empty() ? "No answer" : r.error;
                    return;
                }
                bool ok = false;
                const gv::Json j = gv::Json::parse(r.body, &ok);
                if (!ok)
                {
                    games_error_ = "That address answered, but it isn't GameVault";
                    return;
                }
                games_error_.clear();
                merge_games(j["games"]);
            });
        });
    }

    void merge_games(const gv::Json &list)
    {
        std::map<std::int64_t, Game> old;
        for (Game &g : games_)
            old[g.id] = g;
        std::vector<Game> next;
        for (const gv::Json &x : list.items())
        {
            Game g;
            g.id = x["id"].i64();
            auto it = old.find(g.id);
            if (it != old.end())
            {
                g = it->second;
                old.erase(it);
            }
            const std::string cover = x["cover"].str();
            if (cover != g.cover_url)
            {
                gv::delete_texture(g.cover);
                g.cover = 0;
                g.cover_asked = false;
                g.cover_failed = false;
            }
            g.title = clean(x["title"].str());
            g.platform = x["platform"].str();
            g.title_id = x["titleId"].str();
            g.version = x["version"].str();
            g.cover_url = cover;
            g.added = x["addedAt"].str();
            g.release = x["releaseDate"].str();
            g.required_fw = x["requiredFirmware"].str();
            g.size = x["sizeBytes"].i64();
            g.favourite = x["favourite"].boolean();
            next.push_back(std::move(g));
        }
        for (auto &kv : old)
            gv::delete_texture(kv.second.cover);
        games_.swap(next);
        rebuild_rows();
    }

    void load_cover(Game &g)
    {
        g.cover_asked = true;
        const std::string url = media(g.cover_url);
        if (url.empty())
        {
            g.cover_failed = true;
            return;
        }
        const std::int64_t id = g.id;
        auto work = [this, url, id] {
            gv::HttpResult r = gv::http("GET", url, {}, 15, 8u * 1024u * 1024u);
            auto image = std::make_shared<gv::Image>();
            bool ok = r.ok() && gv::decode_image(r.body, image.get());
            if (ok)
                gv::shrink_to(image.get(), 400);
            images_.post([this, image, ok, id] {
                for (Game &g : games_)
                {
                    if (g.id != id)
                        continue;
                    if (ok)
                    {
                        g.cover = gv::upload_texture(*image);
                        g.cover_w = image->width;
                        g.cover_h = image->height;
                    }
                    else
                        g.cover_failed = true;
                }
            });
        };
        if (snapshot_)
        {
            work();
            images_.run_posted();
        }
        else
            images_.run(work);
    }

    void request_visible_covers()
    {
        if (section_ != Section::library || details_open_)
            return;
        if (images_.pending() > 4)
            return;
        const int first_row = std::max(0, static_cast<int>(scroll_.value / kRowH) - 1);
        const int last = std::min<int>(static_cast<int>(rows_.size()), (first_row + 4) * kColumns);
        for (int i = first_row * kColumns; i < last; ++i)
        {
            Game &g = games_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(i)])];
            if (!g.cover_asked)
            {
                load_cover(g);
                if (images_.pending() > 4)
                    return;
            }
        }
    }

    // A call to the background service, then a refresh.
    void act(const std::string &method, const std::string &path, const std::string &body,
             const std::string &success, int timeout = 15)
    {
        api_.run([this, method, path, body, success, timeout] {
            gv::HttpResult r = gv::http(method, std::string(kService) + path, body, timeout);
            gv::HttpResult s = gv::http("GET", std::string(kService) + "/api/state", {}, 5);
            api_.post([this, r, s, success] {
                if (r.ok())
                {
                    if (!success.empty())
                        toast(success);
                }
                else
                    toast(r.error.empty() ? "GameVault didn't answer" : r.error, true);
                apply_state(s);
            });
        });
    }

    void save_setting(const std::string &key, const std::string &json_value)
    {
        act("PUT", "/api/settings", "{\"" + key + "\":" + json_value + "}", "Saved");
    }

    // Snapshots (PC preview): everything is loaded before the first picture.
    void load_everything_now()
    {
        gv::HttpResult r = gv::http("GET", std::string(kService) + "/api/state", {}, 5);
        apply_state(r);
        if (!service_ok_)
        {
            status_line_ = "GameVault isn't running on this PS5.";
            return;
        }
        gv::HttpResult g = gv::http("GET", server() + "/api/v1/games?apps=hide&sort=title", {}, 10);
        bool ok = false;
        const gv::Json j = gv::Json::parse(g.body, &ok);
        if (ok)
        {
            games_server_ = server();
            merge_games(j["games"]);
        }
    }

    // ---- library ---------------------------------------------------------------------

    void rebuild_rows()
    {
        std::vector<int> rows;
        for (std::size_t i = 0; i < games_.size(); ++i)
        {
            const Game &g = games_[i];
            const GameStatus st = status_of(g, state_);
            const bool on = st.status == Status::downloaded || st.status == Status::update;
            bool keep = true;
            switch (filter_)
            {
            case 1:
                keep = g.platform == "PS5";
                break;
            case 2:
                keep = g.platform == "PS4";
                break;
            case 3:
                keep = on;
                break;
            case 4:
                keep = !on;
                break;
            case 5:
                keep = st.status == Status::update;
                break;
            case 6:
                keep = g.favourite;
                break;
            default:
                break;
            }
            if (keep)
                rows.push_back(static_cast<int>(i));
        }
        std::stable_sort(rows.begin(), rows.end(), [this](int a, int b) {
            const Game &x = games_[static_cast<std::size_t>(a)];
            const Game &y = games_[static_cast<std::size_t>(b)];
            switch (sort_)
            {
            case 1:
                return x.added > y.added;
            case 2:
                return x.release > y.release;
            case 3:
                return x.size > y.size;
            default:
                return x.title < y.title;
            }
        });
        const std::int64_t focused = focused_game_id();
        rows_.swap(rows);
        focus_ = 0;
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (games_[static_cast<std::size_t>(rows_[i])].id == focused)
                focus_ = static_cast<int>(i);
    }

    std::int64_t focused_game_id() const
    {
        if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size()))
            return 0;
        return games_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(focus_)])].id;
    }

    Game *focused_game()
    {
        if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size()))
            return nullptr;
        return &games_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(focus_)])];
    }

    const Game *game_by_id(std::int64_t id) const
    {
        for (const Game &g : games_)
            if (g.id == id)
                return &g;
        return nullptr;
    }

    void update_library(const InputFrame &input, app::Feedback &feedback, float)
    {
        const int count = static_cast<int>(rows_.size());
        if (input.nav != Direction::none && count > 0)
        {
            int next = focus_;
            if (input.nav == Direction::right && focus_ % kColumns < kColumns - 1 && focus_ + 1 < count)
                next = focus_ + 1;
            else if (input.nav == Direction::left && focus_ % kColumns > 0)
                next = focus_ - 1;
            else if (input.nav == Direction::down && focus_ + kColumns < count)
                next = focus_ + kColumns;
            else if (input.nav == Direction::down && focus_ / kColumns < (count - 1) / kColumns)
                next = count - 1;
            else if (input.nav == Direction::up && focus_ - kColumns >= 0)
                next = focus_ - kColumns;
            if (next != focus_)
            {
                focus_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.5f);
        }
        if (input.is_pressed(Action::jump_next) || input.is_pressed(Action::jump_prev))
        {
            filter_ = (filter_ + (input.is_pressed(Action::jump_next) ? 1 : kFilterCount - 1)) % kFilterCount;
            rebuild_rows();
            feedback.play(audio::Cue::tab);
        }
        if (input.is_pressed(Action::west))
        {
            filter_ = (filter_ + 1) % kFilterCount;
            rebuild_rows();
            feedback.play(audio::Cue::toggle);
        }
        if (input.is_pressed(Action::menu))
        {
            sort_ = (sort_ + 1) % kSortCount;
            rebuild_rows();
            feedback.play(audio::Cue::toggle);
        }
        if (input.is_pressed(Action::north))
            toggle_favourite(feedback);
        if (input.is_pressed(Action::confirm) && focused_game())
        {
            open_details(*focused_game());
            feedback.play(audio::Cue::open);
        }
        const int row = count ? focus_ / kColumns : 0;
        scroll_.target = std::max(0.0f, static_cast<float>(row) * kRowH - kRowH * 0.5f);
        if (rows_dirty_)
        {
            rows_dirty_ = false;
            rebuild_rows();
        }
    }

    void toggle_favourite(app::Feedback &feedback)
    {
        Game *g = details_open_ ? find_game(detail_id_) : focused_game();
        if (!g || server().empty())
            return;
        const bool want = !g->favourite;
        g->favourite = want;
        feedback.play(want ? audio::Cue::favorite_on : audio::Cue::favorite_off);
        const std::string url = server() + "/api/v1/games/" + std::to_string(g->id) + "/favourite";
        api_.run([this, url, want] {
            gv::HttpResult r = gv::http("PUT", url, want ? "{\"favourite\":true}" : "{\"favourite\":false}", 10);
            api_.post([this, r] {
                if (!r.ok())
                    toast("Couldn't change the favourite: " + r.error, true);
            });
        });
        if (filter_ == 6)
            rows_dirty_ = true;
    }

    Game *find_game(std::int64_t id)
    {
        for (Game &g : games_)
            if (g.id == id)
                return &g;
        return nullptr;
    }

    void draw_library(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        // Filter and sort line
        float x = kMargin;
        ui::draw_button(list, f, ui::GlyphStyle::dark(), ui::Button::l2, x, 155, 34);
        x += ui::button_width(ui::Button::l2, 34) + 16;
        for (int i = 0; i < kFilterCount; ++i)
        {
            const float w = f.semibold.measure(kFilters[i], 22) + 40;
            const Rect chip{x, 132, w, 46};
            if (i == filter_)
                list.rounded_rect(chip, 23, kInk);
            else
                list.bordered_rect(chip, 23, Color::rgb(0, 0.0f), 2, Color::rgb(0x2A323D));
            ui::text(list, f.semibold, kFilters[i], x + w / 2, 163, 22, i == filter_ ? kBg : kInk2, Align::center);
            x += w + 10;
        }
        ui::draw_button(list, f, ui::GlyphStyle::dark(), ui::Button::r2, x + 6, 155, 34);
        ui::text(list, f.regular, std::string("Sort: ") + kSorts[sort_], kSideX - 40, 163, 21, kMuted, Align::right);

        if (!service_ok_ || server().empty() || (!games_error_.empty() && games_.empty()) || rows_.empty())
        {
            std::string title, body;
            if (!service_ok_)
            {
                title = "Starting GameVault...";
                body = status_line_;
            }
            else if (server().empty())
            {
                title = "Welcome to GameVault";
                body = "Enter your GameVault server's address in Settings (R1 three times), for example http://192.168.0.32:8686.";
            }
            else if (!games_error_.empty() && games_.empty())
            {
                title = "Can't reach GameVault";
                body = games_error_ + ". Check the address in Settings.";
            }
            else if (games_.empty())
            {
                title = "Loading your library...";
            }
            else
            {
                title = "No games match";
                body = "Press □ to change the filter.";
            }
            const Rect panel{kMargin, 250, kSideX - kMargin - 40, 220};
            list.rounded_rect(panel, 20, kPanel);
            ui::text(list, f.semibold, title, panel.x + 40, panel.y + 80, 36, kInk);
            const ui::ParagraphLines lines = ui::paragraph_lines(f.regular, body, 24, panel.w - 80, 3);
            for (std::size_t i = 0; i < lines.lines.size(); ++i)
                ui::text(list, f.regular, lines.lines[i], panel.x + 40, panel.y + 130 + 34 * static_cast<float>(i), 24, kInk2);
            return;
        }

        list.push_clip({0, 196, kSideX - 20, 964 - 196});
        const float scroll = scroll_.value;
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const int col = static_cast<int>(i) % kColumns;
            const int row = static_cast<int>(i) / kColumns;
            const float tx = kMargin + static_cast<float>(col) * (kTileW + kTileGapX);
            const float ty = kGridTop + static_cast<float>(row) * kRowH - scroll;
            if (ty > 1080 || ty + kRowH < 150)
                continue;
            const Game &g = games_[static_cast<std::size_t>(rows_[i])];
            const bool focused = static_cast<int>(i) == focus_;
            const float in = tween::stagger(age_, static_cast<int>(i) % 15);
            list.push_opacity(in);
            draw_tile(list, g, {tx, ty, kTileW, kTileH}, focused);
            list.pop_opacity();
        }
        list.pop_clip();
        // the grid fades out above the footer instead of running into it
        list.gradient_rect({0, 904, 1920, 62}, 0, Color::rgb(0x0F1626, 0.0f), Color::rgb(0x0F1626, 0.95f));
    }

    void draw_cover(gfx::DrawList &list, const Game &g, const Rect &r, float radius, float title_size) const
    {
        const ui::Fonts &f = context_.fonts;
        if (g.cover)
            list.image(g.cover, r, fill_uv(g.cover_w, g.cover_h, r), kWhite, radius);
        else
        {
            static const std::uint32_t palette[] = {0x3A4A3A, 0x6A2D2D, 0x3D4A2A, 0x5A4420, 0x2A3542,
                                                    0x2F3D5C, 0x5C3A22, 0x4A2E4F, 0x2E4766, 0x3B3354};
            std::uint32_t h = 0;
            for (const char c : g.title)
                h = h * 31u + static_cast<unsigned char>(c);
            list.rounded_rect(r, radius, Color::rgb(palette[h % 10]));
            const ui::ParagraphLines lines = ui::paragraph_lines(f.semibold, g.title, title_size, r.w - 28, 4);
            const float lh = title_size * 1.18f;
            float y = r.y + r.h - 18 - lh * static_cast<float>(lines.lines.size() - 1);
            for (const std::string &line : lines.lines)
            {
                ui::text(list, f.semibold, line, r.x + 14, y, title_size, kInk);
                y += lh;
            }
        }
    }

    void draw_badge(gfx::DrawList &list, const GameStatus &st, float right, float top) const
    {
        const ui::Fonts &f = context_.fonts;
        Color fill;
        std::string label;
        switch (st.status)
        {
        case Status::downloaded:
            fill = kGood;
            label = "✓";
            break;
        case Status::update:
            fill = kWarn;
            label = "↑";
            break;
        case Status::downloading:
            fill = kAccent;
            label = st.job && st.job->total > 0
                        ? std::to_string(static_cast<int>(st.job->done * 100 / st.job->total)) + "%"
                        : "...";
            break;
        case Status::queued:
            fill = kAccent;
            label = "Queued";
            break;
        case Status::paused:
            fill = kMuted;
            label = "Paused";
            break;
        case Status::failed:
            fill = kBad;
            label = "!";
            break;
        default:
            return;
        }
        const float w = std::max(40.0f, f.semibold.measure(label, 20) + 24);
        const Rect r{right - w, top, w, 40};
        list.rounded_rect({r.x - 3, r.y - 3, r.w + 6, r.h + 6}, 23, kBg);
        list.rounded_rect(r, 20, fill);
        if (label == "✓")
        {
            list.line(r.x + 12, r.y + 21, r.x + 18, r.y + 28, 4, kBg);
            list.line(r.x + 18, r.y + 28, r.x + 29, r.y + 13, 4, kBg);
        }
        else if (label == "↑")
        {
            list.line(r.x + 20, r.y + 30, r.x + 20, r.y + 11, 4, kBg);
            list.line(r.x + 12, r.y + 18, r.x + 20, r.y + 10, 4, kBg);
            list.line(r.x + 28, r.y + 18, r.x + 20, r.y + 10, 4, kBg);
        }
        else
            ui::text(list, f.semibold, label, r.x + r.w / 2, r.y + 28, 20, kBg, Align::center);
    }

    void draw_tile(gfx::DrawList &list, const Game &g, const Rect &r, bool focused) const
    {
        const ui::Fonts &f = context_.fonts;
        if (focused)
        {
            list.glow(r, 14, 18, kAccent.with_alpha(0.35f));
            list.rounded_rect({r.x - 8, r.y - 8, r.w + 16, r.h + 16}, 20, kWhite);
            list.rounded_rect({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, 16, kBg);
        }
        draw_cover(list, g, r, 12, 22);
        // Platform tag
        const float pw = f.semibold.measure(g.platform, 15) + 18;
        list.rounded_rect({r.x + 10, r.y + 10, pw, 30}, 7, kBg.with_alpha(0.8f));
        ui::text(list, f.semibold, g.platform, r.x + 10 + pw / 2, r.y + 31, 15, kInk, Align::center, 1.0f);
        if (g.favourite)
            list.star(r.x + r.w - 26, r.y + r.h - 26, 13, kWarn);
        const GameStatus st = status_of(g, state_);
        draw_badge(list, st, r.x + r.w - 8, r.y + 8);
        ui::text(list, f.semibold, fit(f.semibold, g.title, 21, r.w), r.x, r.y + r.h + 32, 21, focused ? kInk : kInk2);
        std::string sub = format_bytes(static_cast<double>(g.size));
        Color sub_color = kMuted;
        if (st.status == Status::update)
        {
            sub += " · update v" + g.version;
            sub_color = Color::rgb(0xF5D58A);
        }
        else if (st.status == Status::downloaded)
        {
            sub += " · on this PS5";
            sub_color = Color::rgb(0x8BE5B5);
        }
        ui::text(list, f.regular, fit(f.regular, sub, 18, r.w), r.x, r.y + r.h + 60, 18, sub_color);
    }

    // ---- game page ---------------------------------------------------------------------

    struct DetailButton
    {
        enum Kind
        {
            main,
            trailer,
            favourite,
            remove,
            drive,
        } kind;
        Rect rect;
    };

    // A screenshot on the game page, loaded once the page opens.
    struct Shot
    {
        std::string url;
        std::uint32_t tex = 0;
        int w = 0, h = 0;
        bool failed = false;
    };

    // Rows on the game page, top to bottom (up / down, or L2 / R2, move between them).
    enum DetailRow
    {
        row_actions = 0,
        row_media = 1,
        row_info = 2,
    };

    // Page layout, in page coordinates (the page scrolls under the header).
    static constexpr float kMediaTop = 770.0f;  // "Trailer and screenshots" heading
    static constexpr float kThumbW = 400.0f;
    static constexpr float kThumbH = 225.0f;
    static constexpr float kThumbGap = 22.0f;
    static constexpr float kInfoTop = 1090.0f; // the two information panels

    void clear_detail_media()
    {
        gv::delete_texture(detail_backdrop_);
        detail_backdrop_ = 0;
        detail_backdrop_w_ = detail_backdrop_h_ = 0;
        for (Shot &s : detail_shots_)
            gv::delete_texture(s.tex);
        detail_shots_.clear();
        detail_videos_.clear();
    }

    void open_details(const Game &g)
    {
        details_open_ = true;
        detail_id_ = g.id;
        detail_focus_ = 0;
        detail_row_ = row_actions;
        media_focus_ = 0;
        viewer_open_ = false;
        detail_confirm_delete_ = false;
        detail_ = gv::Json();
        detail_files_ = gv::Json();
        detail_drive_.clear();
        clear_detail_media();
        page_scroll_.value = page_scroll_.target = 0.0f;
        media_scroll_.value = media_scroll_.target = 0.0f;
        const std::string base = server();
        const std::string cover = media(g.cover_url);
        const std::int64_t id = g.id;
        api_.run([this, base, id, cover] {
            gv::HttpResult r = gv::http("GET", base + "/api/v1/games/" + std::to_string(id), {}, 10);
            gv::HttpResult files = gv::http("GET", base + "/api/v1/games/" + std::to_string(id) + "/files", {}, 10);
            bool ok = false;
            gv::Json j = gv::Json::parse(r.body, &ok);
            bool fok = false;
            gv::Json fj = gv::Json::parse(files.body, &fok);
            api_.post([this, j, fj, ok, fok, id] {
                if (detail_id_ != id || !details_open_)
                    return;
                if (ok)
                    detail_ = j;
                if (fok)
                    detail_files_ = fj;
                for (const gv::Json &v : detail_["videos"].items())
                    if (!v["id"].str().empty())
                        detail_videos_.push_back({v["id"].str(), clean(v["name"].str("Trailer"))});
                for (const gv::Json &x : detail_["screenshots"].items())
                {
                    const std::string url = media(x.str());
                    if (!url.empty() && detail_shots_.size() < 8)
                        detail_shots_.push_back(Shot{url});
                }
                for (std::size_t i = 0; i < detail_shots_.size(); ++i)
                    load_shot(id, i);
            });
            // The key art behind the page: the backdrop, or the poster when
            // there's none. Kept large (1920) so it isn't blurry full screen.
            std::string art;
            if (ok && j["backdrop"].str().rfind("/", 0) == 0)
                art = base + j["backdrop"].str();
            else
                art = cover;
            if (art.empty())
                return;
            gv::HttpResult b = gv::http("GET", art, {}, 15, 16u * 1024u * 1024u);
            auto image = std::make_shared<gv::Image>();
            if (!b.ok() || !gv::decode_image(b.body, image.get()))
                return;
            gv::shrink_to(image.get(), 1920);
            api_.post([this, image, id] {
                if (detail_id_ != id || !details_open_)
                    return;
                gv::delete_texture(detail_backdrop_);
                detail_backdrop_ = gv::upload_texture(*image);
                detail_backdrop_w_ = image->width;
                detail_backdrop_h_ = image->height;
            });
        });
    }

    void load_shot(std::int64_t id, std::size_t index)
    {
        const std::string url = detail_shots_[index].url;
        images_.run([this, id, index, url] {
            gv::HttpResult r = gv::http("GET", url, {}, 15, 12u * 1024u * 1024u);
            auto image = std::make_shared<gv::Image>();
            const bool ok = r.ok() && gv::decode_image(r.body, image.get());
            if (ok)
                gv::shrink_to(image.get(), 1280);
            images_.post([this, id, index, image, ok] {
                if (detail_id_ != id || !details_open_ || index >= detail_shots_.size())
                    return;
                Shot &s = detail_shots_[index];
                if (!ok)
                {
                    s.failed = true;
                    return;
                }
                s.tex = gv::upload_texture(*image);
                s.w = image->width;
                s.h = image->height;
            });
        });
    }

    void close_details()
    {
        details_open_ = false;
        viewer_open_ = false;
        clear_detail_media();
    }

    // Trailer first (when the game has one), then the screenshots.
    int media_count() const
    {
        return (detail_videos_.empty() ? 0 : 1) + static_cast<int>(detail_shots_.size());
    }

    bool has_row(int row) const
    {
        return row == row_actions || row == row_info || (row == row_media && media_count() > 0);
    }

    float row_scroll(int row) const
    {
        switch (row)
        {
        case row_media:
            return 300.0f;
        case row_info:
            return 560.0f;
        default:
            return 0.0f;
        }
    }

    void watch_trailer()
    {
        if (detail_videos_.empty())
            return;
        const auto &v = detail_videos_.front();
        const std::string url = "https://www.youtube.com/watch?v=" + v.first;
        ask("Watch \"" + v.second + "\" in the PS5's web browser? GameVault closes while you watch: open it again from the home screen.",
            [this, url] {
                api_.run([this, url] {
                    gv::HttpResult r = gv::http("POST", std::string(kService) + "/api/open", "{\"url\":" + gv::json_quote(url) + "}", 5);
                    api_.post([this, r] {
                        if (!r.ok())
                        {
                            toast("Couldn't open the trailer: " + (r.error.empty() ? std::string("GameVault didn't answer") : r.error), true);
                            return;
                        }
                        sys::log("[GV] closing for the trailer");
                        sys::quit();
                    });
                });
            });
    }

    std::string detail_drive() const
    {
        std::vector<std::string> present;
        for (const Drive &d : state_.drives)
            present.push_back(d.id);
        auto has = [&](const std::string &id) {
            return std::find(present.begin(), present.end(), id) != present.end();
        };
        if (!detail_drive_.empty() && has(detail_drive_))
            return detail_drive_;
        if (has(state_.ps5_drive))
            return state_.ps5_drive;
        return "internal";
    }

    const Drive *drive_by_id(const std::string &id) const
    {
        for (const Drive &d : state_.drives)
            if (d.id == id)
                return &d;
        return nullptr;
    }

    double free_on(const std::string &id) const
    {
        const Drive *d = drive_by_id(id);
        if (!d)
            return 0;
        auto it = state_.reserved.find(id);
        return std::max(0.0, d->free - (it != state_.reserved.end() ? it->second : 0.0));
    }

    struct MainAction
    {
        std::string label, note;
        Color fill, ink, note_color;
        bool enabled = true;
        enum
        {
            none,
            download,
            update,
            resume,
            downloads,
            storage,
        } go = none;
    };

    MainAction main_action(const Game &g) const
    {
        MainAction a;
        const GameStatus st = status_of(g, state_);
        const bool ps4 = g.platform == "PS4";
        const std::string drive = ps4 ? "internal" : detail_drive();
        const double free = free_on(drive);
        const double need = static_cast<double>(g.size);
        const std::string fw = detail_["requiredFirmware"].str(g.required_fw);
        const bool incompatible = !ps4 && !fw.empty() && !state_.firmware.empty() && fw_compare(fw, state_.firmware) > 0;
        const bool folder_ps4 = detail_files_["kind"].str() == "ps4-folder";
        const int pct = st.job && st.job->total > 0 ? static_cast<int>(st.job->done * 100 / st.job->total) : 0;
        a.note_color = kMuted;
        a.fill = kAccent;
        a.ink = kBg;
        switch (st.status)
        {
        case Status::downloading:
        case Status::queued:
            a.label = st.status == Status::queued ? "Queued" : "Downloading · " + std::to_string(pct) + "%";
            a.note = "Open Downloads (R1) to pause or cancel.";
            a.go = MainAction::downloads;
            return a;
        case Status::paused:
            a.label = "Resume · " + std::to_string(pct) + "%";
            a.note = "Paused. Resuming carries on from where it stopped.";
            a.go = MainAction::resume;
            return a;
        case Status::downloaded:
            a.label = "Downloaded";
            a.fill = Color::rgb(0x3FCF8E, 0.16f);
            a.ink = Color::rgb(0x8BE5B5);
            a.enabled = false;
            a.note = std::string("On this PS5 (") + drive_name(st.installed->drive) + ")" +
                     (st.installed->version.empty() ? "" : " · v" + st.installed->version) + ", the same version as on the NAS.";
            a.note_color = Color::rgb(0x8BE5B5);
            return a;
        default:
            break;
        }
        if (folder_ps4)
        {
            a.label = "Can't install";
            a.fill = Color::rgb(0xF07A7A, 0.14f);
            a.ink = Color::rgb(0xFFB4B4);
            a.enabled = false;
            a.note = "This PS4 game is a folder dump; the PS5 installs PS4 games only from .pkg files.";
            a.note_color = Color::rgb(0xFFB4B4);
            return a;
        }
        if (incompatible)
        {
            a.label = "Needs firmware " + fw;
            a.fill = Color::rgb(0xF07A7A, 0.14f);
            a.ink = Color::rgb(0xFFB4B4);
            a.enabled = false;
            a.note = "This version needs firmware " + fw + " or later; this PS5 is on " + state_.firmware + ".";
            a.note_color = Color::rgb(0xFFB4B4);
            return a;
        }
        if (need > free)
        {
            a.label = "Too large";
            a.fill = Color::rgb(0xF07A7A, 0.14f);
            a.ink = Color::rgb(0xFFB4B4);
            a.go = MainAction::storage;
            a.note = "Needs " + format_bytes(need) + "; " + drive_name(drive) + " has " + format_bytes(free) +
                     " free. Free up space in Storage" + (ps4 ? "." : ", or change the drive.");
            a.note_color = Color::rgb(0xFFB4B4);
            return a;
        }
        if (st.status == Status::update)
        {
            a.label = "Update · " + format_bytes(need);
            a.fill = kWarn;
            a.go = MainAction::update;
            a.note = "On this PS5 as v" + st.installed->version + ". The NAS has v" + g.version + ".";
            a.note_color = Color::rgb(0xF5D58A);
            return a;
        }
        a.label = ps4 ? "Install" : "Download";
        a.go = MainAction::download;
        if (st.status == Status::failed && st.job)
        {
            a.note = "The last attempt failed: " + st.job->error;
            a.note_color = Color::rgb(0xFFB4B4);
        }
        else if (ps4)
            a.note = "The PS5 installs PS4 games itself, to the drive in its own Storage settings.";
        else
            a.note = std::string("Fits on ") + drive_name(drive) + ": " + format_bytes(free) + " free, " +
                     format_bytes(need) + " needed.";
        return a;
    }

    std::vector<DetailButton::Kind> detail_buttons(const Game &g) const
    {
        std::vector<DetailButton::Kind> b{DetailButton::main};
        if (!detail_videos_.empty())
            b.push_back(DetailButton::trailer);
        b.push_back(DetailButton::favourite);
        const GameStatus st = status_of(g, state_);
        if (g.platform == "PS5" && state_.drives.size() > 1 && st.status != Status::downloaded)
            b.push_back(DetailButton::drive);
        if (st.installed && st.installed->kind != "ps4-app" && st.status != Status::downloading)
            b.push_back(DetailButton::remove);
        return b;
    }

    void move_row(int step, app::Feedback &feedback)
    {
        int row = detail_row_;
        do
            row += step;
        while (row >= row_actions && row <= row_info && !has_row(row));
        if (row < row_actions || row > row_info)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.5f);
            return;
        }
        detail_row_ = row;
        feedback.play(audio::Cue::focus);
    }

    void update_viewer(const InputFrame &input, app::Feedback &feedback)
    {
        const int count = static_cast<int>(detail_shots_.size());
        if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm) || count == 0)
        {
            viewer_open_ = false;
            feedback.play(audio::Cue::back);
            return;
        }
        const int step = input.nav == Direction::right || input.is_pressed(Action::jump_next) ? 1
                         : input.nav == Direction::left || input.is_pressed(Action::jump_prev) ? -1
                                                                                               : 0;
        if (step != 0)
        {
            viewer_index_ = (viewer_index_ + step + count) % count;
            feedback.play(audio::Cue::focus);
        }
    }

    void update_details(const InputFrame &input, app::Feedback &feedback)
    {
        Game *g = find_game(detail_id_);
        if (viewer_open_)
        {
            update_viewer(input, feedback);
            return;
        }
        if (!g || input.is_pressed(Action::back))
        {
            close_details();
            feedback.play(audio::Cue::back);
            return;
        }
        if (!has_row(detail_row_))
            detail_row_ = row_actions;
        if (input.nav == Direction::down || input.is_pressed(Action::jump_next))
            move_row(1, feedback);
        else if (input.nav == Direction::up || input.is_pressed(Action::jump_prev))
            move_row(-1, feedback);
        page_scroll_.target = row_scroll(detail_row_);

        if (input.is_pressed(Action::north))
            toggle_favourite(feedback);
        if (input.is_pressed(Action::west) && g->platform == "PS5")
            cycle_drive(feedback);

        if (detail_row_ == row_media)
        {
            const int count = media_count();
            if (input.nav == Direction::left && media_focus_ > 0)
            {
                --media_focus_;
                feedback.play(audio::Cue::focus);
            }
            else if (input.nav == Direction::right && media_focus_ + 1 < count)
            {
                ++media_focus_;
                feedback.play(audio::Cue::focus);
            }
            media_focus_ = std::max(0, std::min(media_focus_, count - 1));
            // Keep the focused picture on screen (the row is wider than the page).
            const float x = static_cast<float>(media_focus_) * (kThumbW + kThumbGap);
            const float view = 1920 - 2 * kMargin;
            if (x - media_scroll_.target < 0)
                media_scroll_.target = x;
            else if (x + kThumbW - media_scroll_.target > view)
                media_scroll_.target = x + kThumbW - view;
            if (input.is_pressed(Action::confirm))
            {
                const bool trailer = !detail_videos_.empty() && media_focus_ == 0;
                if (trailer)
                    watch_trailer();
                else
                {
                    viewer_index_ = media_focus_ - (detail_videos_.empty() ? 0 : 1);
                    viewer_open_ = true;
                    feedback.play(audio::Cue::open);
                }
            }
            return;
        }
        if (detail_row_ != row_actions)
            return;

        const auto buttons = detail_buttons(*g);
        if (input.nav == Direction::left && detail_focus_ > 0)
        {
            --detail_focus_;
            detail_confirm_delete_ = false;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::right && detail_focus_ + 1 < static_cast<int>(buttons.size()))
        {
            ++detail_focus_;
            detail_confirm_delete_ = false;
            feedback.play(audio::Cue::focus);
        }
        detail_focus_ = std::min(detail_focus_, static_cast<int>(buttons.size()) - 1);
        if (!input.is_pressed(Action::confirm))
            return;
        switch (buttons[static_cast<std::size_t>(detail_focus_)])
        {
        case DetailButton::trailer:
            watch_trailer();
            break;
        case DetailButton::favourite:
            toggle_favourite(feedback);
            break;
        case DetailButton::drive:
            cycle_drive(feedback);
            break;
        case DetailButton::remove: {
            const GameStatus st = status_of(*g, state_);
            if (!st.installed)
                break;
            const std::string path = st.installed->path, title = g->title;
            ask("Delete " + title + " (" + format_bytes(st.installed->size) + ") from this PS5? The copy on your NAS stays.",
                [this, path, title] { act("DELETE", "/api/installed?path=" + gv::url_encode(path), "", "Deleted " + title); });
            break;
        }
        case DetailButton::main: {
            const MainAction a = main_action(*g);
            if (!a.enabled)
            {
                feedback.play(audio::Cue::error);
                break;
            }
            feedback.play(audio::Cue::select);
            const GameStatus st = status_of(*g, state_);
            const std::string drive = g->platform == "PS4" ? "" : detail_drive();
            std::string body = "{\"gameId\":" + std::to_string(g->id);
            if (!drive.empty())
                body += ",\"drive\":" + gv::json_quote(drive);
            switch (a.go)
            {
            case MainAction::download:
                act("POST", "/api/downloads", body + "}",
                    g->platform == "PS4" ? "Sending " + g->title + " to the PS5's installer" : "Downloading " + g->title);
                break;
            case MainAction::update:
                if (st.installed && st.installed->kind != "ps4-app")
                    body += ",\"replace\":" + gv::json_quote(st.installed->path);
                act("POST", "/api/downloads", body + "}", "Updating " + g->title);
                break;
            case MainAction::resume:
                if (st.job)
                    act("POST", "/api/downloads/" + st.job->id + "/resume", "", "Resuming");
                break;
            case MainAction::downloads:
                close_details();
                section_ = Section::downloads;
                break;
            case MainAction::storage:
                close_details();
                section_ = Section::storage;
                break;
            default:
                break;
            }
            break;
        }
        }
    }

    void cycle_drive(app::Feedback &feedback)
    {
        if (state_.drives.size() < 2)
            return;
        const std::string now = detail_drive();
        std::size_t i = 0;
        for (; i < state_.drives.size(); ++i)
            if (state_.drives[i].id == now)
                break;
        detail_drive_ = state_.drives[(i + 1) % state_.drives.size()].id;
        feedback.play(audio::Cue::toggle);
    }

    static std::string nice_date(const std::string &iso)
    {
        static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-')
            return iso;
        const int m = std::atoi(iso.substr(5, 2).c_str());
        const int d = std::atoi(iso.substr(8, 2).c_str());
        if (m < 1 || m > 12 || d < 1)
            return iso;
        return std::to_string(d) + " " + months[m - 1] + " " + iso.substr(0, 4);
    }

    static std::string kind_name(const std::string &kind)
    {
        if (kind == "ps5-exfat")
            return "PS5 exFAT image";
        if (kind == "ps5-folder")
            return "PS5 folder dump";
        if (kind == "ps4-pkg")
            return "PS4 package (.pkg)";
        if (kind == "ps4-folder")
            return "PS4 folder dump";
        return kind;
    }

    std::string joined(const char *key) const
    {
        std::string out;
        for (const gv::Json &x : detail_[key].items())
            if (!x.str().empty())
                out += (out.empty() ? "" : ", ") + clean(x.str());
        return out;
    }

    void draw_info_panel(gfx::DrawList &list, const Rect &r, const char *title,
                         const std::vector<std::pair<std::string, std::string>> &rows, bool focused) const
    {
        const ui::Fonts &f = context_.fonts;
        list.bordered_rect(r, 18, kPanel.with_alpha(0.92f), focused ? 2.0f : 1.0f, focused ? kAccent2 : kLine);
        ui::text(list, f.semibold, title, r.x + 32, r.y + 52, 26, kInk);
        float y = r.y + 104;
        for (const auto &row : rows)
        {
            if (row.second.empty())
                continue;
            ui::text(list, f.regular, row.first, r.x + 32, y, 20, kMuted);
            ui::text(list, f.semibold, fit(f.semibold, row.second, 21, r.w - 280), r.x + 240, y, 21, kInk);
            y += 44;
            if (y > r.y + r.h - 16)
                break;
        }
    }

    void draw_details(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        const Game *g = game_by_id(detail_id_);
        if (!g)
            return;
        const float sc = page_scroll_.value;
        const Rect full{0, 0, 1920, 1080};

        // Key art: the whole screen, cropped to fit (never stretched), shaded
        // on the left and at the bottom so the text over it stays readable.
        if (detail_backdrop_)
        {
            list.image(detail_backdrop_, full, fill_uv(detail_backdrop_w_, detail_backdrop_h_, full, 0.5f, 0.3f),
                       kWhite.with_alpha(0.62f));
            list.gradient_rect_h(full, 0, kBg.with_alpha(0.97f), kBg.with_alpha(0.2f));
            list.gradient_rect({0, 420, 1920, 660}, 0, kBg.with_alpha(0.0f), kBg.with_alpha(0.98f));
            list.rounded_rect(full, 0, kBg.with_alpha(std::min(0.55f, sc / 900.0f)));
        }
        // the header sits on a dark band, which the page scrolls under
        list.rounded_rect({0, 0, 1920, 112}, 0, kBg.with_alpha(0.96f));
        list.gradient_rect({0, 112, 1920, 70}, 0, kBg.with_alpha(0.96f), kBg.with_alpha(0.0f));

        list.push_clip({0, 118, 1920, 960 - 118});
        const float top = -sc; // page y -> screen y

        ui::text(list, f.regular, "Library  /  " + fit(f.regular, g->title, 22, 900), kMargin, 168 + top, 22, kInk2);

        // Poster, cropped to its frame
        const Rect cover{kMargin, 205 + top, 300, 450};
        list.shadow(cover, 16, 40, Color::rgb(0, 0.55f));
        draw_cover(list, *g, cover, 16, 28);

        const float tx = cover.x + cover.w + 52;
        const float tw = 1920 - tx - kMargin;
        float y = 240 + top;
        const float pw = f.semibold.measure(g->platform, 17) + 24;
        list.rounded_rect({tx, y - 26, pw, 34}, 8, kBg.with_alpha(0.8f));
        ui::text(list, f.semibold, g->platform, tx + pw / 2, y - 3, 17, kInk, Align::center, 1.0f);
        std::string genres;
        for (const gv::Json &x : detail_["genres"].items())
            genres += (genres.empty() ? "" : " · ") + clean(x.str());
        if (!genres.empty())
        {
            const float gw = f.regular.measure(genres, 17) + 24;
            list.rounded_rect({tx + pw + 10, y - 26, gw, 34}, 8, kWhite.with_alpha(0.14f));
            ui::text(list, f.regular, genres, tx + pw + 22, y - 3, 17, kInk2);
        }
        y += 74;
        const ui::ParagraphLines title = ui::paragraph_lines(f.display, g->title, 62, tw, 2);
        for (const std::string &line : title.lines)
        {
            ui::text(list, f.display, line, tx, y, 62, kInk);
            y += 70;
        }
        std::string meta;
        auto add = [&](const std::string &s) {
            if (!s.empty())
                meta += (meta.empty() ? "" : " · ") + s;
        };
        add(detail_["year"].str());
        add(clean(detail_["developer"].str()));
        if (detail_["publisher"].str() != detail_["developer"].str())
            add(clean(detail_["publisher"].str()));
        if (!detail_["criticScore"].is_null())
            add("Critic score " + detail_["criticScore"].str());
        ui::text(list, f.regular, fit(f.regular, meta, 22, tw), tx, y, 22, kInk2);
        y += 42;
        const ui::ParagraphLines summary =
            ui::paragraph_lines(f.regular, clean(detail_["summary"].str()), 21, std::min(tw, 1150.0f), 3);
        for (const std::string &line : summary.lines)
        {
            ui::text(list, f.regular, line, tx, y, 21, kInk2);
            y += 31;
        }

        // Buttons
        const MainAction a = main_action(*g);
        const auto buttons = detail_buttons(*g);
        float bx = tx;
        const float by = std::max(y + 22.0f, 540.0f + top);
        for (std::size_t i = 0; i < buttons.size(); ++i)
        {
            const bool focused = detail_row_ == row_actions && static_cast<int>(i) == detail_focus_;
            std::string label;
            Color fill = kWhite.with_alpha(0.14f), ink = kInk;
            float w = 0;
            switch (buttons[i])
            {
            case DetailButton::main:
                label = a.label;
                fill = a.fill;
                ink = a.ink;
                w = f.semibold.measure(label, 26) + 72;
                break;
            case DetailButton::trailer:
                label = "Watch trailer";
                w = f.semibold.measure(label, 24) + 88;
                break;
            case DetailButton::favourite:
                label = "Favourite";
                w = f.semibold.measure(label, 24) + 82;
                break;
            case DetailButton::drive:
                label = std::string("Save to: ") + drive_name(detail_drive());
                w = f.semibold.measure(label, 24) + 56;
                break;
            case DetailButton::remove:
                label = "Delete from PS5";
                w = f.semibold.measure(label, 24) + 56;
                break;
            }
            const Rect r{bx, by, w, 76};
            if (focused)
            {
                list.rounded_rect({r.x - 6, r.y - 6, r.w + 12, r.h + 12}, 22, kWhite);
                list.rounded_rect({r.x - 3, r.y - 3, r.w + 6, r.h + 6}, 19, kBg);
            }
            list.rounded_rect(r, 16, fill);
            if (buttons[i] == DetailButton::favourite)
            {
                list.star(r.x + 34, r.y + 38, 13, kWarn, g->favourite ? 0.0f : 2.5f);
                ui::text(list, f.semibold, label, r.x + 58, r.y + 47, 24, ink);
            }
            else if (buttons[i] == DetailButton::trailer)
            {
                list.triangle({r.x + 28, r.y + 26, 24, 24}, ink, 0.0f, 1.5708f);
                ui::text(list, f.semibold, label, r.x + 64, r.y + 47, 24, ink);
            }
            else
                ui::text(list, f.semibold, label, r.x + r.w / 2, r.y + 48, buttons[i] == DetailButton::main ? 26 : 24, ink,
                         Align::center);
            bx += w + 18;
        }
        ui::text(list, f.regular, fit(f.regular, a.note, 21, tw), tx, by + 116, 21, a.note_color);

        // Trailer and screenshots
        const int count = media_count();
        if (count > 0)
        {
            const float my = kMediaTop + top;
            ui::text(list, f.semibold, "Trailer and screenshots", kMargin, my, 28, kInk);
            const std::string n = std::to_string(detail_shots_.size()) + (detail_shots_.size() == 1 ? " screenshot" : " screenshots") +
                                  (detail_videos_.empty() ? "" : " · trailer");
            ui::text(list, f.regular, n, 1920 - kMargin, my, 20, kMuted, Align::right);
            list.push_clip({kMargin - 14, my + 10, 1920 - 2 * kMargin + 28, kThumbH + 40});
            for (int i = 0; i < count; ++i)
            {
                const Rect r{kMargin + static_cast<float>(i) * (kThumbW + kThumbGap) - media_scroll_.value, my + 30, kThumbW, kThumbH};
                if (r.x > 1920 || r.x + r.w < 0)
                    continue;
                const bool focused = detail_row_ == row_media && i == media_focus_;
                if (focused)
                {
                    list.rounded_rect({r.x - 7, r.y - 7, r.w + 14, r.h + 14}, 18, kWhite);
                    list.rounded_rect({r.x - 3, r.y - 3, r.w + 6, r.h + 6}, 15, kBg);
                }
                const bool trailer = !detail_videos_.empty() && i == 0;
                const Shot *shot = trailer ? (detail_shots_.empty() ? nullptr : &detail_shots_[0])
                                           : &detail_shots_[static_cast<std::size_t>(i - (detail_videos_.empty() ? 0 : 1))];
                if (trailer && detail_backdrop_)
                    list.image(detail_backdrop_, r, fill_uv(detail_backdrop_w_, detail_backdrop_h_, r), kWhite, 12);
                else if (shot && shot->tex)
                    list.image(shot->tex, r, fill_uv(shot->w, shot->h, r), kWhite, 12);
                else
                {
                    list.rounded_rect(r, 12, Color::rgb(0x1A2230));
                    if (!trailer)
                        ui::text(list, f.regular, shot && shot->failed ? "Couldn't load" : "Loading", r.x + r.w / 2,
                                 r.y + r.h / 2 + 8, 19, kMuted, Align::center);
                }
                if (trailer)
                {
                    list.gradient_rect(r, 12, Color::rgb(0, 0.15f), Color::rgb(0, 0.75f));
                    list.circle(r.x + r.w / 2, r.y + r.h / 2 - 12, 38, kWhite.with_alpha(0.92f));
                    list.triangle({r.x + r.w / 2 - 12, r.y + r.h / 2 - 30, 34, 36}, kBg, 0.0f, 1.5708f);
                    ui::text(list, f.semibold, fit(f.semibold, detail_videos_.front().second, 20, r.w - 32), r.x + 16,
                             r.y + r.h - 18, 20, kInk);
                }
            }
            list.pop_clip();
        }

        // File and game information, as on the desktop page
        const float iy = (count > 0 ? kInfoTop : kMediaTop) + top;
        const float pw2 = (1920 - 2 * kMargin - 28) / 2;
        const GameStatus st = status_of(*g, state_);
        const std::string fw_need = detail_["requiredFirmware"].str(g->required_fw);
        std::string on_ps5 = "No";
        if (st.installed)
            on_ps5 = std::string("Yes, ") + drive_name(st.installed->drive) +
                     (st.installed->version.empty() ? "" : " · v" + st.installed->version);
        const std::string files = detail_["fileCount"].is_null() ? "" : detail_["fileCount"].str();
        const std::vector<std::pair<std::string, std::string>> file_rows = {
            {"Format", kind_name(detail_["kind"].str(detail_files_["kind"].str()))},
            {"Size", format_bytes(static_cast<double>(g->size))},
            {"Files", files == "0" ? "" : files},
            {"Title ID", g->title_id},
            {"Version on NAS", g->version.empty() ? "" : "v" + g->version},
            {"On this PS5", on_ps5},
            {"Needs firmware", fw_need.empty() ? "" : fw_need + (state_.firmware.empty() ? "" : " (this PS5: " + state_.firmware + ")")},
        };
        std::string critic = detail_["criticScore"].is_null() ? "" : detail_["criticScore"].str() + " / 100";
        std::string user = detail_["userScore"].is_null() ? "" : detail_["userScore"].str() + " / 100";
        const std::vector<std::pair<std::string, std::string>> game_rows = {
            {"Release date", nice_date(detail_["releaseDate"].str(g->release))},
            {"Developer", clean(detail_["developer"].str())},
            {"Publisher", clean(detail_["publisher"].str())},
            {"Genres", joined("genres")},
            {"Modes", joined("modes")},
            {"Critic score", critic},
            {"User rating", user},
        };
        const bool info_focus = detail_row_ == row_info;
        draw_info_panel(list, {kMargin, iy, pw2, 420}, "File information", file_rows, info_focus);
        draw_info_panel(list, {kMargin + pw2 + 28, iy, pw2, 420}, "Game information", game_rows, info_focus);

        list.pop_clip();
    }

    // A screenshot, full screen. Left / right (or L2 / R2) browse, ○ closes.
    void draw_viewer(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x05070A));
        if (viewer_index_ < 0 || viewer_index_ >= static_cast<int>(detail_shots_.size()))
            return;
        const Shot &s = detail_shots_[static_cast<std::size_t>(viewer_index_)];
        const Rect area{80, 50, 1760, 900};
        if (s.tex && s.w > 0 && s.h > 0)
        {
            // fit the whole picture inside the area
            const float scale = std::min(area.w / static_cast<float>(s.w), area.h / static_cast<float>(s.h));
            const float w = static_cast<float>(s.w) * scale, h = static_cast<float>(s.h) * scale;
            list.image(s.tex, {area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h}, gfx::kFullUv, kWhite, 10);
        }
        else
            ui::text(list, f.regular, s.failed ? "Couldn't load this screenshot" : "Loading", 960, 520, 26, kMuted, Align::center);
        const std::string n = std::to_string(viewer_index_ + 1) + " / " + std::to_string(detail_shots_.size());
        ui::text(list, f.semibold, n, 1920 - kMargin, 1018, 24, kInk2, Align::right);
        const ui::Hint hints[] = {{ui::Button::dpad, "Browse"}, {ui::Button::circle, "Close"}};
        ui::draw_hints(list, f, ui::GlyphStyle::dark(), hints, 2, kMargin, false);
    }

    // ---- downloads -------------------------------------------------------------------

    void update_downloads(const InputFrame &input, app::Feedback &feedback)
    {
        const int count = static_cast<int>(state_.jobs.size());
        move_focus(input, feedback, list_focus_, count);
        if (count == 0)
            return;
        list_focus_ = std::min(list_focus_, count - 1);
        const Job &j = state_.jobs[static_cast<std::size_t>(list_focus_)];
        const bool running = j.state == "downloading" || j.state == "verifying" || j.state == "queued";
        if (input.is_pressed(Action::confirm))
        {
            if (running && j.platform == "PS5")
                act("POST", "/api/downloads/" + j.id + "/pause", "", "Paused");
            else if (j.state == "paused" || j.state == "failed")
                act("POST", "/api/downloads/" + j.id + "/resume", "", "Resuming");
            feedback.play(audio::Cue::select);
        }
        if (input.is_pressed(Action::west) && (running || j.state == "paused" || j.state == "failed"))
        {
            const std::string id = j.id, title = j.title;
            ask("Cancel downloading " + title + "? What's downloaded so far is deleted.",
                [this, id] { act("POST", "/api/downloads/" + id + "/cancel", "", "Cancelled"); });
        }
        if (input.is_pressed(Action::north) && (j.state == "done" || j.state == "cancelled" || j.state == "failed"))
        {
            act("DELETE", "/api/downloads/" + j.id, "", "");
            feedback.play(audio::Cue::erase);
        }
        if (input.is_pressed(Action::menu))
        {
            act("POST", "/api/downloads/clear", "", "Cleared finished downloads");
            feedback.play(audio::Cue::erase);
        }
    }

    void move_focus(const InputFrame &input, app::Feedback &feedback, int &focus, int count)
    {
        if (count <= 0)
        {
            focus = 0;
            return;
        }
        if (input.nav == Direction::down && focus + 1 < count)
        {
            ++focus;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::up && focus > 0)
        {
            --focus;
            feedback.play(audio::Cue::focus);
        }
    }

    void draw_progress(gfx::DrawList &list, const Rect &r, double fraction, Color color, double extra = 0, Color extra_color = {}) const
    {
        list.rounded_rect(r, r.h / 2, kLine);
        const float w = static_cast<float>(std::clamp(fraction, 0.0, 1.0)) * r.w;
        if (w > 0)
        {
            list.push_clip({r.x, r.y, w, r.h});
            list.rounded_rect(r, r.h / 2, color);
            list.pop_clip();
        }
        if (extra > 0)
        {
            const float e = static_cast<float>(std::clamp(extra, 0.0, 1.0 - fraction)) * r.w;
            list.push_clip({r.x + w, r.y, e, r.h});
            list.rounded_rect(r, r.h / 2, extra_color);
            list.pop_clip();
        }
    }

    void draw_downloads(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        ui::text(list, f.display, "Downloads", kMargin, 190, 48, kInk);
        if (state_.game_running && state_.play_limit > 0)
            ui::text(list, f.regular, "A game is running, so downloads are limited to " + std::to_string(state_.play_limit) + " MB/s.",
                     kMargin + 320, 188, 21, Color::rgb(0xF5D58A));
        if (state_.jobs.empty())
        {
            const Rect panel{kMargin, 240, kSideX - kMargin - 40, 150};
            list.rounded_rect(panel, 20, kPanel);
            ui::text(list, f.regular, "Nothing here yet. Pick a game in the Library and press Download.", panel.x + 36, panel.y + 86, 24, kInk2);
            return;
        }
        constexpr float kRow = 132.0f;
        const float width = kSideX - kMargin - 40;
        list.push_clip({0, 222, kSideX - 20, 760});
        const float scroll = std::max(0.0f, static_cast<float>(list_focus_) * kRow - 3 * kRow);
        for (std::size_t i = 0; i < state_.jobs.size(); ++i)
        {
            const Job &j = state_.jobs[i];
            const Rect r{kMargin, 240 + static_cast<float>(i) * kRow - scroll, width, kRow - 16};
            if (r.y > 1000 || r.y + r.h < 200)
                continue;
            const bool focused = static_cast<int>(i) == list_focus_;
            if (focused)
                list.rounded_rect({r.x - 5, r.y - 5, r.w + 10, r.h + 10}, 22, kWhite);
            list.rounded_rect(r, 18, focused ? Color::rgb(0x161B24) : kPanel);
            const float pw = f.semibold.measure(j.platform, 15) + 18;
            list.rounded_rect({r.x + 24, r.y + 20, pw, 28}, 7, kBg);
            ui::text(list, f.semibold, j.platform, r.x + 24 + pw / 2, r.y + 40, 15, kInk, Align::center);
            ui::text(list, f.semibold, fit(f.semibold, j.title, 26, r.w - 500), r.x + 36 + pw, r.y + 44, 26, kInk);
            std::string line;
            const std::string state_word = j.state == "queued" ? "Waiting" : j.state == "downloading" ? "Downloading"
                                           : j.state == "verifying" ? "Checking the file" : j.state == "paused" ? "Paused"
                                           : j.state == "done" ? "Done" : j.state == "failed" ? "Failed" : "Cancelled";
            line = state_word;
            if (j.platform == "PS5")
            {
                line += std::string(" · ") + drive_name(j.drive);
                if (j.state != "done")
                    line += " · " + format_bytes(j.done) + " of " + format_bytes(j.total);
                else
                    line += " · " + format_bytes(j.total);
                if (j.state == "downloading" && j.speed > 0)
                    line += " · " + format_bytes(j.speed) + "/s · " + format_eta(j.total - j.done, j.speed);
            }
            ui::text(list, f.regular, fit(f.regular, line, 20, r.w - 60), r.x + 24, r.y + 80, 20, kMuted);
            if (!j.error.empty())
                ui::text(list, f.regular, fit(f.regular, j.error, 19, r.w - 60), r.x + 24, r.y + 106, 19, Color::rgb(0xFFB4B4));
            else if (!j.note.empty())
                ui::text(list, f.regular, fit(f.regular, j.note, 19, r.w - 60), r.x + 24, r.y + 106, 19, kMuted);
            if (j.platform == "PS5" && j.state != "done" && j.state != "cancelled")
            {
                const Color c = j.state == "failed" ? kBad : j.state == "paused" ? kMuted : kAccent;
                draw_progress(list, {r.x + r.w - 420, r.y + 34, 300, 12}, j.total > 0 ? j.done / j.total : 0, c);
                ui::text(list, f.semibold, std::to_string(j.total > 0 ? static_cast<int>(j.done * 100 / j.total) : 0) + "%",
                         r.x + r.w - 30, r.y + 46, 24, kInk, Align::right);
            }
        }
        list.pop_clip();
        ui::text(list, f.regular, "Downloads carry on in the background while you play, and after this app is closed.",
                 kMargin, 1000 - 40, 19, kMuted);
    }

    // ---- storage ---------------------------------------------------------------------

    void update_storage(const InputFrame &input, app::Feedback &feedback)
    {
        const int count = static_cast<int>(state_.installed.size());
        move_focus(input, feedback, list_focus2_, count);
        if (count == 0 || !input.is_pressed(Action::confirm))
            return;
        list_focus2_ = std::min(list_focus2_, count - 1);
        const std::vector<Installed> items = sorted_installed();
        const Installed &it = items[static_cast<std::size_t>(list_focus2_)];
        if (it.kind == "ps4-app")
        {
            toast("Delete installed PS4 games from the PS5's own Storage settings.");
            feedback.play(audio::Cue::error);
            return;
        }
        const std::string path = it.path, title = it.title.empty() ? it.title_id : it.title;
        ask("Delete " + title + " (" + format_bytes(it.size) + ") from this PS5? The copy on your NAS stays.",
            [this, path, title] { act("DELETE", "/api/installed?path=" + gv::url_encode(path), "", "Deleted " + title); });
    }

    std::vector<Installed> sorted_installed() const
    {
        std::vector<Installed> v = state_.installed;
        std::stable_sort(v.begin(), v.end(), [](const Installed &a, const Installed &b) { return a.size > b.size; });
        return v;
    }

    void draw_storage(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        ui::text(list, f.display, "Storage", kMargin, 190, 48, kInk);
        const float width = kSideX - kMargin - 40;
        float y = 230;
        const float dw = (width - 20.0f * static_cast<float>(state_.drives.size() - 1)) / static_cast<float>(std::max<std::size_t>(1, state_.drives.size()));
        float x = kMargin;
        for (const Drive &d : state_.drives)
        {
            double games = 0;
            int count = 0;
            for (const Installed &i : state_.installed)
                if (i.drive == d.id)
                {
                    games += i.size;
                    ++count;
                }
            const Rect r{x, y, dw, 150};
            list.rounded_rect(r, 18, kPanel);
            ui::text(list, f.semibold, d.label, r.x + 24, r.y + 44, 24, kInk);
            ui::text(list, f.semibold, format_bytes(d.free) + " free", r.x + r.w - 24, r.y + 44, 24, kInk, Align::right);
            const double used = d.total - d.free;
            draw_progress(list, {r.x + 24, r.y + 66, r.w - 48, 14}, d.total > 0 ? (used - games) / d.total : 0,
                          Color::rgb(0x3A4554), d.total > 0 ? games / d.total : 0, kAccent);
            ui::text(list, f.regular,
                     format_bytes(used) + " of " + format_bytes(d.total) + " used · blue: " + std::to_string(count) +
                         (count == 1 ? " game" : " games"),
                     r.x + 24, r.y + 118, 18, kMuted);
            x += dw + 20;
        }
        y += 190;
        ui::text(list, f.semibold, "Games on this PS5", kMargin, y, 26, kInk);
        y += 24;
        const std::vector<Installed> items = sorted_installed();
        if (items.empty())
        {
            ui::text(list, f.regular, "No games found in the homebrew folders yet.", kMargin, y + 50, 22, kMuted);
            return;
        }
        constexpr float kRow = 86.0f;
        list.push_clip({0, y, kSideX - 20, 940 - y});
        const float scroll = std::max(0.0f, static_cast<float>(list_focus2_) * kRow - 3 * kRow);
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const Installed &it = items[i];
            const Rect r{kMargin, y + 10 + static_cast<float>(i) * kRow - scroll, width, kRow - 12};
            if (r.y > 1000)
                break;
            const bool focused = static_cast<int>(i) == list_focus2_;
            if (focused)
                list.rounded_rect({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, 18, kWhite);
            list.rounded_rect(r, 14, focused ? Color::rgb(0x161B24) : kPanel);
            ui::text(list, f.semibold, fit(f.semibold, it.title.empty() ? it.title_id : it.title, 22, r.w - 420), r.x + 22, r.y + 34, 22, kInk);
            const std::string kind = it.kind == "image" ? "exFAT image" : it.kind == "folder" ? "Folder" : "Installed PS4 game";
            ui::text(list, f.regular, it.title_id + (it.version.empty() ? "" : " · v" + it.version) + " · " + kind + " · " + drive_name(it.drive),
                     r.x + 22, r.y + 60, 17, kMuted);
            ui::text(list, f.semibold, format_bytes(it.size), r.x + r.w - 22, r.y + 46, 22, kInk2, Align::right);
        }
        list.pop_clip();
    }

    // ---- settings --------------------------------------------------------------------

    std::vector<SettingRow> setting_rows() const
    {
        std::vector<SettingRow> rows{SettingRow::server,     SettingRow::test,       SettingRow::drive,
                                     SettingRow::connections, SettingRow::speed_test, SettingRow::play_limit,
                                     SettingRow::notify,     SettingRow::check_files};
        if (state_.update_ready)
            rows.push_back(SettingRow::update);
        rows.push_back(SettingRow::stop);
        return rows;
    }

    void update_settings(const InputFrame &input, app::Feedback &feedback)
    {
        const auto rows = setting_rows();
        move_focus(input, feedback, settings_focus_, static_cast<int>(rows.size()));
        settings_focus_ = std::min(settings_focus_, static_cast<int>(rows.size()) - 1);
        const SettingRow row = rows[static_cast<std::size_t>(settings_focus_)];
        const int step = input.nav == Direction::right ? 1 : input.nav == Direction::left ? -1 : 0;
        const bool confirm = input.is_pressed(Action::confirm);
        switch (row)
        {
        case SettingRow::server:
            if (confirm)
            {
                prompt_.set_title("GameVault server address");
                prompt_.field.set_placeholder("http://192.168.0.32:8686");
                prompt_.field.set_helper("The NAS's IP address and GameVault's port");
                prompt_.open(feedback, state_.server.empty() ? "http://" : state_.server);
            }
            break;
        case SettingRow::test:
            if (confirm)
            {
                feedback.play(audio::Cue::select);
                const std::string base = server();
                test_result_ = "Testing...";
                api_.run([this, base] {
                    gv::HttpResult r = gv::http("GET", base + "/api/v1/health", {}, 8);
                    bool ok = false;
                    gv::Json j = gv::Json::parse(r.body, &ok);
                    std::string msg = r.ok() && ok
                                          ? "Connected · GameVault " + j["version"].str() + " · " + j["games"].str() + " games"
                                          : (r.error.empty() ? "No answer" : r.error);
                    const bool good = r.ok() && ok;
                    api_.post([this, msg, good] {
                        test_result_ = msg;
                        test_good_ = good;
                    });
                });
            }
            break;
        case SettingRow::drive:
            if (step != 0 && !state_.drives.empty())
            {
                std::size_t i = 0;
                for (; i < state_.drives.size(); ++i)
                    if (state_.drives[i].id == state_.ps5_drive)
                        break;
                const std::size_t n = state_.drives.size();
                const std::size_t next = (i + n + static_cast<std::size_t>(step + static_cast<int>(n))) % n;
                state_.ps5_drive = state_.drives[next].id;
                save_setting("ps5Drive", gv::json_quote(state_.ps5_drive));
                feedback.play(audio::Cue::toggle);
            }
            break;
        case SettingRow::connections:
            if (step != 0)
            {
                static const int kValues[] = {0, 1, 2, 3, 4, 6, 8};
                int i = 0;
                while (i < 6 && kValues[i] != state_.connections)
                    ++i;
                i = std::clamp(i + step, 0, 6);
                state_.connections = kValues[i];
                save_setting("connections", std::to_string(state_.connections));
                feedback.play(audio::Cue::toggle);
            }
            break;
        case SettingRow::speed_test:
            if (confirm && !speed_running_)
            {
                const Game *biggest = nullptr;
                for (const Game &g : games_)
                    if (g.platform == "PS5" && (!biggest || g.size > biggest->size))
                        biggest = &g;
                if (!biggest)
                {
                    toast("No PS5 game to test with", true);
                    break;
                }
                speed_running_ = true;
                speed_result_ = "Testing... about 16 seconds";
                feedback.play(audio::Cue::select);
                const std::string body = "{\"gameId\":" + std::to_string(biggest->id) + "}";
                api_.run([this, body] {
                    gv::HttpResult r = gv::http("POST", std::string(kService) + "/api/speed-test", body, 60);
                    gv::HttpResult s = gv::http("GET", std::string(kService) + "/api/state", {}, 5);
                    std::string msg;
                    if (r.ok())
                    {
                        bool ok = false;
                        gv::Json j = gv::Json::parse(r.body, &ok);
                        for (const gv::Json &x : j["results"].items())
                        {
                            char buf[64];
                            std::snprintf(buf, sizeof buf, "%s%d: %.0f MB/s", msg.empty() ? "" : " · ",
                                          static_cast<int>(x["connections"].i64()), x["mbps"].num());
                            msg += buf;
                        }
                        msg += "  -  Automatic uses " + j["best"].str();
                    }
                    else
                        msg = r.error;
                    api_.post([this, msg, s] {
                        speed_running_ = false;
                        speed_result_ = msg;
                        apply_state(s);
                    });
                });
            }
            break;
        case SettingRow::play_limit:
            if (step != 0)
            {
                static const int kValues[] = {0, 5, 10, 20, 50};
                int i = 0;
                while (i < 4 && kValues[i] != state_.play_limit)
                    ++i;
                i = std::clamp(i + step, 0, 4);
                state_.play_limit = kValues[i];
                save_setting("playLimitMBps", std::to_string(state_.play_limit));
                feedback.play(audio::Cue::toggle);
            }
            break;
        case SettingRow::notify:
            if (confirm || step != 0)
            {
                state_.notify = !state_.notify;
                save_setting("notify", state_.notify ? "true" : "false");
                feedback.play(audio::Cue::toggle);
            }
            break;
        case SettingRow::check_files:
            if (confirm || step != 0)
            {
                state_.check_files = !state_.check_files;
                save_setting("checkFiles", state_.check_files ? "true" : "false");
                feedback.play(audio::Cue::toggle);
            }
            break;
        case SettingRow::update:
            if (confirm)
                ask("Restart GameVault as version " + state_.elf_version + "? A download in progress pauses for a moment and carries on.",
                    [this] {
                        toast("Restarting GameVault...");
                        act("POST", "/api/update", "", "", 10);
                    });
            break;
        case SettingRow::stop:
            if (confirm)
                ask("Stop GameVault on this PS5? Downloads pause until it's started again (open this app, or load gamevault.elf).",
                    [this] { act("POST", "/api/quit", "", "GameVault stopped"); });
            break;
        }
    }

    void draw_settings(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        ui::text(list, f.display, "Settings", kMargin, 190, 48, kInk);
        const auto rows = setting_rows();
        const float width = 1920 - 2 * kMargin;
        constexpr float kRow = 82.0f;
        const float scroll = std::max(0.0f, static_cast<float>(settings_focus_) * kRow - 5 * kRow);
        list.push_clip({0, 214, 1920, 712});
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const Rect r{kMargin, 232 + static_cast<float>(i) * kRow - scroll, width, kRow - 12};
            const bool focused = static_cast<int>(i) == settings_focus_;
            if (focused)
                list.rounded_rect({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, 18, kWhite);
            list.rounded_rect(r, 14, focused ? Color::rgb(0x161B24) : kPanel);
            std::string label, value, help;
            bool arrows = false;
            switch (rows[i])
            {
            case SettingRow::server:
                label = "GameVault server";
                value = state_.server.empty() ? "Not set" : state_.server;
                help = "✕ to edit";
                break;
            case SettingRow::test:
                label = "Test connection";
                value = test_result_;
                help = "✕";
                break;
            case SettingRow::drive: {
                label = "PS5 games: save to";
                const Drive *d = drive_by_id(state_.ps5_drive);
                value = std::string(drive_name(state_.ps5_drive)) + (d ? " · " + format_bytes(d->free) + " free" : " · not connected");
                arrows = true;
                break;
            }
            case SettingRow::connections:
                label = "Connections per download";
                value = state_.connections == 0
                            ? "Automatic (" + std::to_string(state_.measured_connections ? state_.measured_connections : 2) + ")"
                            : std::to_string(state_.connections);
                arrows = true;
                break;
            case SettingRow::speed_test:
                label = "Test download speed";
                value = !speed_result_.empty() ? speed_result_
                        : state_.measured_connections
                            ? "Last test: " + std::to_string(state_.measured_mbps) + " MB/s with " + std::to_string(state_.measured_connections)
                            : "Picks the connections for Automatic";
                help = "✕";
                break;
            case SettingRow::play_limit:
                label = "While a game is running, limit downloads to";
                value = state_.play_limit ? std::to_string(state_.play_limit) + " MB/s" : "No limit";
                arrows = true;
                break;
            case SettingRow::notify:
                label = "Notification when a download finishes";
                value = state_.notify ? "On" : "Off";
                arrows = true;
                break;
            case SettingRow::check_files:
                label = "Check each image after it downloads";
                value = state_.check_files ? "On" : "Off";
                arrows = true;
                break;
            case SettingRow::update:
                label = "Update GameVault";
                value = "Version " + state_.elf_version + " is ready (this is " + state_.version + ")";
                help = "✕";
                break;
            case SettingRow::stop:
                label = "Stop GameVault";
                value = "Stops the background service";
                help = "✕";
                break;
            }
            ui::text(list, f.semibold, label, r.x + 28, r.y + 45, 24, kInk);
            Color vc = rows[i] == SettingRow::test ? (test_good_ ? Color::rgb(0x8BE5B5) : kInk2) : kInk2;
            if (rows[i] == SettingRow::update)
                vc = kAccent2;
            float vx = r.x + r.w - 28;
            if (arrows)
            {
                // < value >
                list.triangle({vx - 14, r.y + 28, 14, 14}, focused ? kInk : kMuted, 0.0f, 1.5708f);
                vx -= 30;
            }
            const std::string shown = fit(f.regular, value, 22, r.w * 0.5f);
            const float vw = ui::text(list, f.regular, shown, vx, r.y + 45, 22, vc, Align::right);
            if (arrows)
                list.triangle({vx - vw - 30, r.y + 28, 14, 14}, focused ? kInk : kMuted, 0.0f, -1.5708f);
            (void)help;
        }
        list.pop_clip();
        std::string about = "GameVault " + state_.version + " · PS5 firmware " + state_.firmware +
                            " · also open http://<PS5 address>:8687 on a phone or PC. PS4 games install to the console's own install location.";
        ui::text(list, f.regular, fit(f.regular, about, 19, 1920 - 2 * kMargin), kMargin, 960, 19, kMuted);
    }

    // ---- shared pieces ---------------------------------------------------------------

    void ask(const std::string &text, std::function<void()> action)
    {
        confirm_text_ = text;
        confirm_action_ = std::move(action);
        confirm_open_ = true;
    }

    void draw_header(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        // The V mark
        const float mx = kMargin, my = 44, s = 0.7f;
        const float left[] = {mx + 7 * s, my + 7 * s, mx + 20 * s, my + 7 * s, mx + 37 * s, my + 57 * s, mx + 25 * s, my + 57 * s};
        const float right[] = {mx + 57 * s, my + 7 * s, mx + 44 * s, my + 7 * s, mx + 27 * s, my + 57 * s, mx + 39 * s, my + 57 * s};
        list.polygon(left, 4, kAccent);
        list.polygon(right, 4, kAccent2);
        const float w = ui::text(list, f.semibold, "Game", mx + 56, my + 34, 32, kInk);
        ui::text(list, f.semibold, "Vault", mx + 56 + w, my + 34, 32, kAccent);

        float x = 340;
        ui::draw_button(list, f, ui::GlyphStyle::dark(), ui::Button::l1, x, 66, 34);
        x += ui::button_width(ui::Button::l1, 34) + 20;
        int active = 0;
        for (const Job &j : state_.jobs)
            if (active_state(j.state))
                ++active;
        for (int i = 0; i < kSections; ++i)
        {
            const bool current = static_cast<int>(section_) == i;
            const float tw = f.semibold.measure(kSectionNames[i], 23) + 44 + (i == 1 && active ? 40 : 0);
            const Rect pill{x, 42, tw, 50};
            if (current)
                list.rounded_rect(pill, 25, kInk);
            ui::text(list, f.semibold, kSectionNames[i], x + 22, 76, 23, current ? kBg : kInk2);
            if (i == 1 && active)
            {
                const float cx = x + tw - 36;
                list.rounded_rect({cx, 54, 28, 26}, 13, kAccent);
                ui::text(list, f.semibold, std::to_string(active), cx + 14, 74, 16, kBg, Align::center);
            }
            x += tw + 8;
        }
        ui::draw_button(list, f, ui::GlyphStyle::dark(), ui::Button::r1, x + 12, 66, 34);

        // Connection status, top right
        Color dot = kGood;
        std::string text;
        if (!service_ok_)
        {
            dot = kWarn;
            text = "Starting GameVault...";
        }
        else if (server().empty())
        {
            dot = kWarn;
            text = "GameVault server not set";
        }
        else if (!games_error_.empty())
        {
            dot = kBad;
            text = "Can't reach GameVault";
        }
        else
            text = "GameVault connected · " + std::to_string(games_.size()) + " games";
        if (state_.update_ready)
            text = "Update to " + state_.elf_version + " in Settings · " + text;
        const float tw = f.regular.measure(text, 21);
        list.circle(1920 - kMargin - tw - 18, 66, 6, dot);
        ui::text(list, f.regular, text, 1920 - kMargin, 74, 21, kInk2, Align::right);
    }

    // The footer's right side: free space on each drive, and what's downloading.
    // (It was a panel beside the library; down here the grid gets two more columns.)
    void draw_status_strip(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        constexpr float cy = 1010.0f;
        float x = 1920 - kMargin;

        // Downloads, rightmost
        const Job *current = nullptr;
        int waiting = 0;
        for (const Job &j : state_.jobs)
        {
            if (!active_state(j.state))
                continue;
            if (!current || (current->state != "downloading" && j.state == "downloading"))
                current = &j;
            ++waiting;
        }
        if (current)
        {
            --waiting;
            const double frac = current->total > 0 ? current->done / current->total : 0;
            std::string line = current->state == "paused"      ? "Paused"
                               : current->state == "queued"    ? "Waiting"
                               : current->state == "verifying" ? "Checking"
                                                               : format_bytes(current->speed) + "/s";
            if (waiting > 0)
                line += " · " + std::to_string(waiting) + " more";
            x -= f.regular.measure(line, 19);
            ui::text(list, f.regular, line, x, cy + 7, 19, kMuted);
            x -= 16 + 110;
            draw_progress(list, {x, cy - 4, 110, 8}, frac, kAccent);
            const std::string title = fit(f.semibold, current->title, 20, 300) + " " +
                                      std::to_string(static_cast<int>(frac * 100)) + "%";
            x -= 14 + f.semibold.measure(title, 20);
            ui::text(list, f.semibold, title, x, cy + 7, 20, kInk);
            // a small arrow, moving while it downloads
            const float bob = current->state == "downloading" ? std::sin(clock_ * 5.0f) * 2.0f : 0.0f;
            x -= 26;
            list.line(x + 9, cy - 10 + bob, x + 9, cy + 6 + bob, 3, kAccent);
            list.line(x + 2, cy + bob, x + 9, cy + 7 + bob, 3, kAccent);
            list.line(x + 16, cy + bob, x + 9, cy + 7 + bob, 3, kAccent);
        }
        else
        {
            const char *idle = "Nothing downloading";
            x -= f.regular.measure(idle, 19);
            ui::text(list, f.regular, idle, x, cy + 7, 19, kMuted);
        }

        // Drives, to the left of it
        for (auto it = state_.drives.rbegin(); it != state_.drives.rend(); ++it)
        {
            const Drive &d = *it;
            x -= 28;
            list.rounded_rect({x, cy - 16, 2, 32}, 1, kLine);
            x -= 28;
            const std::string free = format_bytes(d.free) + " free";
            x -= f.semibold.measure(free, 19);
            ui::text(list, f.semibold, free, x, cy + 7, 19, kInk);
            x -= 12 + 90;
            draw_progress(list, {x, cy - 4, 90, 8}, d.total > 0 ? (d.total - d.free) / d.total : 0,
                          d.id == "internal" ? kAccent : kAccent2);
            x -= 12 + f.regular.measure(d.label, 19);
            ui::text(list, f.regular, d.label, x, cy + 7, 19, kInk2);
            if (x < 1060) // keep clear of the button hints
                break;
        }
    }

    void draw_hints(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        std::vector<ui::Hint> hints;
        if (details_open_)
        {
            const bool trailer = detail_row_ == row_media && !detail_videos_.empty() && media_focus_ == 0;
            if (detail_row_ == row_media)
                hints = {{ui::Button::cross, trailer ? "Watch trailer" : "View"}, {ui::Button::dpad, "Browse"}};
            else if (detail_row_ == row_actions)
                hints = {{ui::Button::cross, "Select"}};
            hints.push_back({ui::Button::triangle, "Favourite"});
            const Game *g = game_by_id(detail_id_);
            if (g && g->platform == "PS5" && state_.drives.size() > 1)
                hints.push_back({ui::Button::square, "Change drive"});
            hints.push_back({ui::Button::l2, "More", ui::Button::r2});
            hints.push_back({ui::Button::circle, "Back"});
        }
        else
            switch (section_)
            {
            case Section::library:
                hints = {{ui::Button::cross, "Open"}, {ui::Button::triangle, "Favourite"}, {ui::Button::l2, "Filter", ui::Button::r2},
                         {ui::Button::options, "Sort"}};
                break;
            case Section::downloads:
                hints = {{ui::Button::cross, "Pause / resume"}, {ui::Button::square, "Cancel"}, {ui::Button::triangle, "Remove"},
                         {ui::Button::options, "Clear finished"}};
                break;
            case Section::storage:
                hints = {{ui::Button::cross, "Delete game"}};
                break;
            case Section::settings:
                hints = {{ui::Button::cross, "Select"}, {ui::Button::dpad, "Change"}};
                break;
            }
        if (!details_open_)
            hints.push_back({ui::Button::l1, "Section", ui::Button::r1});
        ui::draw_hints(list, f, ui::GlyphStyle::dark(), hints.data(), static_cast<int>(hints.size()), kMargin, false);
    }

    void draw_toast(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        const float a = std::min(1.0f, toast_time_ * 3.0f);
        const float w = std::min(1300.0f, f.semibold.measure(toast_, 24) + 64);
        const Rect r{960 - w / 2, 900, w, 68};
        list.push_opacity(a);
        list.shadow(r, 16, 30, Color::rgb(0, 0.5f));
        list.bordered_rect(r, 16, Color::rgb(0x1C232D), 1, toast_error_ ? Color::rgb(0x7A3434) : Color::rgb(0x2A323D));
        ui::text(list, f.semibold, fit(f.semibold, toast_, 24, w - 48), 960, r.y + 43, 24,
                 toast_error_ ? Color::rgb(0xFFB4B4) : kInk, Align::center);
        list.pop_opacity();
    }

    void draw_confirm(gfx::DrawList &list) const
    {
        const ui::Fonts &f = context_.fonts;
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0, 0.6f));
        const Rect r{460, 360, 1000, 330};
        list.shadow(r, 24, 50, Color::rgb(0, 0.6f));
        list.bordered_rect(r, 24, kPanel, 1, kLine);
        const ui::ParagraphLines lines = ui::paragraph_lines(f.semibold, confirm_text_, 28, r.w - 96, 4);
        float y = r.y + 80;
        for (const std::string &l : lines.lines)
        {
            ui::text(list, f.semibold, l, r.x + 48, y, 28, kInk);
            y += 40;
        }
        const ui::Hint hints[] = {{ui::Button::cross, "Yes"}, {ui::Button::circle, "No"}};
        ui::HintLayout layout;
        layout.cy = r.y + r.h - 50;
        ui::draw_hints(list, f, ui::GlyphStyle::dark(), hints, 2, r.x + 48, false, layout);
    }

    // ---- state ---------------------------------------------------------------------

    app::Context &context_;
    gv::Workers api_;
    gv::Workers images_;
    bool snapshot_ = false;

    float age_ = 0.0f;
    float clock_ = 0.0f;
    bool splash_on_ = true;
    float splash_age_ = 0.0f;
    float splash_out_ = 0.0f;
    float section_age_ = 0.0f;
    Section section_ = Section::library;

    ServiceState state_;
    bool service_ok_ = false;
    bool start_tried_ = false;
    bool state_busy_ = false;
    float since_state_ = 0.0f;
    std::string status_line_;

    std::vector<Game> games_;
    std::vector<int> rows_;
    bool games_busy_ = false;
    bool rows_dirty_ = false;
    float since_games_ = 0.0f;
    std::string games_server_;
    std::string games_error_;
    int filter_ = 0;
    int sort_ = 0;
    int focus_ = 0;
    tween::Spring scroll_;

    bool details_open_ = false;
    std::int64_t detail_id_ = 0;
    int detail_focus_ = 0;
    bool detail_confirm_delete_ = false;
    gv::Json detail_;
    gv::Json detail_files_;
    std::string detail_drive_;
    std::uint32_t detail_backdrop_ = 0;
    int detail_backdrop_w_ = 0, detail_backdrop_h_ = 0;
    std::vector<Shot> detail_shots_;
    std::vector<std::pair<std::string, std::string>> detail_videos_; // YouTube id, name
    int detail_row_ = 0;
    int media_focus_ = 0;
    tween::Spring page_scroll_;
    tween::Spring media_scroll_;
    bool viewer_open_ = false;
    int viewer_index_ = 0;

    int list_focus_ = 0;  // downloads
    int list_focus2_ = 0; // storage
    tween::Spring list_scroll_;
    int settings_focus_ = 0;
    std::string test_result_;
    bool test_good_ = false;
    std::string speed_result_;
    bool speed_running_ = false;

    std::string toast_;
    bool toast_error_ = false;
    float toast_time_ = 0.0f;

    bool confirm_open_ = false;
    std::string confirm_text_;
    std::function<void()> confirm_action_;

    ui::InputPrompt prompt_;
};

} // namespace

std::unique_ptr<app::Concept> make_gamevault(app::Context &context)
{
    return std::make_unique<GameVault>(context);
}

} // namespace hui::concepts
