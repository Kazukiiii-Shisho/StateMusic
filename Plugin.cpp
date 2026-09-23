
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <reframework/API.hpp>
#include <sol/sol.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <windows.h>

using namespace reframework;

namespace {

std::mutex g_mtx;
ma_engine g_engine;
bool g_engine_ok = false;

struct Slot {
    ma_sound sound{};
    bool ok = false;
};

Slot g_slots[2];
int g_active = -1;
std::string g_current_path;

bool ensure_engine() {
    if (g_engine_ok) {
        return true;
    }
    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS) {
        return false;
    }
    g_engine_ok = true;
    return true;
}

void free_slot(Slot& s) {
    if (s.ok) {
        ma_sound_uninit(&s.sound);
        s.ok = false;
    }
}

std::wstring to_wide(const std::string& s, UINT cp) {
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string ansi_to_utf8(const std::string& s) {
    return wide_to_utf8(to_wide(s, CP_ACP));
}

bool init_sound_any(const std::string& path, ma_uint32 flags, ma_sound* out) {
    std::wstring w = to_wide(path, CP_UTF8);
    if (!w.empty()
        && ma_sound_init_from_file_w(&g_engine, w.c_str(), flags, nullptr, nullptr, out) == MA_SUCCESS) {
        return true;
    }
    std::wstring wa = to_wide(path, CP_ACP);
    if (!wa.empty() && wa != w
        && ma_sound_init_from_file_w(&g_engine, wa.c_str(), flags, nullptr, nullptr, out) == MA_SUCCESS) {
        return true;
    }
    return false;
}

void seek_seconds(ma_sound* s, float sec) {
    if (sec <= 0.0f) {
        return;
    }
    ma_uint64 len_frames = 0;
    float len_sec = 0.0f;
    if (ma_sound_get_length_in_pcm_frames(s, &len_frames) != MA_SUCCESS || len_frames == 0) {
        return;
    }
    if (ma_sound_get_length_in_seconds(s, &len_sec) != MA_SUCCESS || len_sec <= 0.0f) {
        return;
    }
    if (sec >= len_sec) {
        return;
    }
    ma_uint64 frame = (ma_uint64)((double)sec * (double)len_frames / (double)len_sec);
    ma_sound_seek_to_pcm_frame(s, frame);
}

void set_sound_loop_points(ma_sound* s, float start_sec, float end_sec) {
    ma_uint64 len_frames = 0;
    float len_sec = 0.0f;
    if (ma_sound_get_length_in_pcm_frames(s, &len_frames) != MA_SUCCESS || len_frames == 0) {
        return;
    }
    if (ma_sound_get_length_in_seconds(s, &len_sec) != MA_SUCCESS || len_sec <= 0.0f) {
        return;
    }
    const double fps = (double)len_frames / (double)len_sec;
    ma_uint64 beg = (start_sec > 0.0f) ? (ma_uint64)((double)start_sec * fps) : 0;
    ma_uint64 end = (end_sec > 0.0f && end_sec < len_sec)
        ? (ma_uint64)((double)end_sec * fps) : len_frames;
    if (end <= beg) {
        return;
    }
    ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(s), beg, end);
}

void audio_seek(float sec) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return;
    }
    if (sec <= 0.0f) {
        ma_sound_seek_to_pcm_frame(&g_slots[g_active].sound, 0);
        return;
    }
    seek_seconds(&g_slots[g_active].sound, sec);
}

float audio_get_length(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (path.empty() || !ensure_engine()) {
        return -1.0f;
    }
    ma_sound probe;
    if (!init_sound_any(path, MA_SOUND_FLAG_STREAM, &probe)) {
        return -1.0f;
    }
    float len = -1.0f;
    if (ma_sound_get_length_in_seconds(&probe, &len) != MA_SUCCESS) {
        len = -1.0f;
    }
    ma_sound_uninit(&probe);
    return len;
}

void audio_set_loop_points(float start_sec, float end_sec) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return;
    }
    ma_sound_set_looping(&g_slots[g_active].sound, MA_TRUE);
    set_sound_loop_points(&g_slots[g_active].sound, start_sec, end_sec);
}

bool audio_set_intro_loop(float intro_start, float body_start, float body_end) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return false;
    }
    ma_sound* s = &g_slots[g_active].sound;
    ma_sound_set_looping(s, MA_TRUE);
    set_sound_loop_points(s, body_start, body_end);
    float cur = -1.0f;
    ma_sound_get_cursor_in_seconds(s, &cur);
    if (cur < 0.0f || cur <= body_start + 0.05f) {
        if (intro_start <= 0.0f) {
            ma_sound_seek_to_pcm_frame(s, 0);
        } else {
            seek_seconds(s, intro_start);
        }
    }
    return true;
}

static ma_uint32 sound_flags(bool decode) {
    const ma_uint32 base = MA_SOUND_FLAG_NO_SPATIALIZATION;
    return base | (decode ? (MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC) : MA_SOUND_FLAG_STREAM);
}

bool audio_play(const std::string& path, bool loop, float gain, float start_sec, bool decode) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (path.empty() || !ensure_engine()) {
        return false;
    }
    for (auto& s : g_slots) {
        free_slot(s);
    }
    g_active = -1;

    Slot& s = g_slots[0];
    if (!init_sound_any(path, sound_flags(decode), &s.sound)) {
        return false;
    }
    s.ok = true;
    ma_sound_set_looping(&s.sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(&s.sound, gain);
    seek_seconds(&s.sound, start_sec);
    ma_sound_start(&s.sound);
    g_active = 0;
    g_current_path = path;
    return true;
}

bool audio_fade_to(const std::string& path, bool loop, float seconds, float gain, float start_sec,
                   bool decode) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (path.empty() || !ensure_engine()) {
        return false;
    }
    if (path == g_current_path && g_active >= 0) {
        return true;
    }

    const ma_uint64 ms = (ma_uint64)(seconds * 1000.0f);
    const int next = (g_active == 0) ? 1 : 0;

    Slot& ns = g_slots[next];
    free_slot(ns);
    if (!init_sound_any(path, sound_flags(decode), &ns.sound)) {
        return false;
    }
    ns.ok = true;
    ma_sound_set_looping(&ns.sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(&ns.sound, gain);
    seek_seconds(&ns.sound, start_sec);
    ma_sound_set_fade_in_milliseconds(&ns.sound, 0.0f, 1.0f, ms);
    ma_sound_start(&ns.sound);

    if (g_active >= 0 && g_slots[g_active].ok) {
        ma_sound_set_fade_in_milliseconds(&g_slots[g_active].sound, -1.0f, 0.0f, ms);
    }

    g_active = next;
    g_current_path = path;
    return true;
}

void audio_stop() {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& s : g_slots) {
        free_slot(s);
    }
    g_active = -1;
    g_current_path.clear();
}

void audio_fade_out(float seconds) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_engine_ok) {
        return;
    }
    if (seconds <= 0.02f || g_active < 0 || !g_slots[g_active].ok) {
        for (auto& s : g_slots) {
            free_slot(s);
        }
        g_active = -1;
        g_current_path.clear();
        return;
    }
    Slot& s = g_slots[g_active];
    const ma_uint64 ms = (ma_uint64)(seconds * 1000.0f);
    ma_sound_set_fade_in_milliseconds(&s.sound, -1.0f, 0.0f, ms);
    const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&g_engine);
    const ma_uint64 sr = ma_engine_get_sample_rate(&g_engine);
    ma_sound_set_stop_time_in_pcm_frames(&s.sound, now + (ma_uint64)((double)sr * (double)seconds));
    g_active = -1;
    g_current_path.clear();
}

void audio_set_volume(float v) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_engine_ok) {
        ma_engine_set_volume(&g_engine, v);
    }
}

void audio_set_active_volume(float gain) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active >= 0 && g_slots[g_active].ok) {
        ma_sound_set_volume(&g_slots[g_active].sound, gain);
    }
}

bool audio_is_playing() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_active >= 0 && g_slots[g_active].ok;
}

bool audio_is_ready() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return false;
    }
    ma_uint64 len = 0;
    return ma_sound_get_length_in_pcm_frames(&g_slots[g_active].sound, &len) == MA_SUCCESS
        && len > 0;
}

float audio_remaining() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return -1.0f;
    }
    float cursor = 0.0f;
    float length = 0.0f;
    if (ma_sound_get_cursor_in_seconds(&g_slots[g_active].sound, &cursor) != MA_SUCCESS) {
        return -1.0f;
    }
    if (ma_sound_get_length_in_seconds(&g_slots[g_active].sound, &length) != MA_SUCCESS) {
        return -1.0f;
    }
    if (length <= 0.0f) {
        return -1.0f;
    }
    const float rem = length - cursor;
    return rem < 0.0f ? 0.0f : rem;
}

float audio_get_cursor() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return -1.0f;
    }
    float cur = 0.0f;
    if (ma_sound_get_cursor_in_seconds(&g_slots[g_active].sound, &cur) != MA_SUCCESS) {
        return -1.0f;
    }
    return cur;
}

bool audio_at_end() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_active < 0 || !g_slots[g_active].ok) {
        return false;
    }
    return ma_sound_at_end(&g_slots[g_active].sound) == MA_TRUE;
}


static bool has_unsafe_cmd_char(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20) return true;
        switch (c) {
            case '"': case '&': case '|': case '<': case '>':
            case '^': case '%': case '`':
                return true;
            default: break;
        }
    }
    return false;
}

static bool is_safe_url(const std::string& url) {
    if (url.empty() || url.size() > 2048) return false;
    const bool https = url.rfind("https://", 0) == 0;
    const bool http = url.rfind("http://", 0) == 0;
    if (!https && !http) return false;
    return !has_unsafe_cmd_char(url);
}

static bool is_safe_path(const std::string& p) {
    if (p.empty() || p.size() > 4096) return false;
    for (unsigned char c : p) {
        if (c < 0x20) return false;
        switch (c) {
            case '"': case '&': case '|': case '<': case '>':
            case '^': case '`':
                return false;
            default: break;
        }
    }
    return true;
}

std::atomic<int> g_dl_status{0};

static std::string safe_audio_format(const std::string& fmt) {
    static const char* allowed[] = { "mp3", "wav", "flac", "vorbis", "m4a", "best" };
    for (const char* a : allowed) {
        if (fmt == a) {
            return fmt;
        }
    }
    return "mp3";
}

void audio_download(const std::string& url, const std::string& outtemplate, bool playlist,
                    const std::string& format) {
    if (g_dl_status.load() == 1 || url.empty() || outtemplate.empty()) {
        return;
    }
    if (!is_safe_url(url) || !is_safe_path(outtemplate)) {
        g_dl_status = 3;
        return;
    }
    g_dl_status = 1;
    const std::string fmt = safe_audio_format(format);
    std::thread([url, outtemplate, playlist, fmt]() {
        const char* pl = playlist ? "--yes-playlist" : "--no-playlist";
        std::string cmd = std::string("yt-dlp -x --audio-format ") + fmt
            + " --restrict-filenames " + pl
            + " -o \"" + outtemplate + "\" \"" + url + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::string mutable_cmd = cmd;
        BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!ok) {
            g_dl_status = 4;
            return;
        }
        DWORD wait = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
        DWORD code = 1;
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            code = 1;
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        g_dl_status = (code == 0) ? 2 : 3;
    }).detach();
}

std::atomic<int> g_fetch_status{0};

void audio_fetch(const std::string& url, const std::string& outpath) {
    if (g_fetch_status.load() == 1 || url.empty() || outpath.empty()) {
        return;
    }
    if (!is_safe_url(url) || !is_safe_path(outpath)) {
        g_fetch_status = 3;
        return;
    }
    g_fetch_status = 1;
    std::thread([url, outpath]() {
        std::string cmd = "curl -s -L --max-time 10 -o \"" + outpath + "\" \"" + url + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::string mutable_cmd = cmd;
        BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!ok) {
            g_fetch_status = 3;
            return;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        g_fetch_status = (code == 0) ? 2 : 3;
    }).detach();
}

void on_lua_state_created(lua_State* l) {
    API::LuaLock lock{};
    sol::state_view lua{l};
    auto t = lua.create_named_table("audio");
    t["play"] = [](const char* path, sol::optional<bool> loop, sol::optional<float> gain, sol::optional<float> start_sec, sol::optional<bool> decode) {
        return audio_play(path ? path : "", loop.value_or(true), gain.value_or(1.0f), start_sec.value_or(0.0f), decode.value_or(false));
    };
    t["fade_to"] = [](const char* path, sol::optional<bool> loop, sol::optional<float> sec, sol::optional<float> gain, sol::optional<float> start_sec, sol::optional<bool> decode) {
        return audio_fade_to(path ? path : "", loop.value_or(true), sec.value_or(2.0f), gain.value_or(1.0f), start_sec.value_or(0.0f), decode.value_or(false));
    };
    t["stop"] = []() { audio_stop(); };
    t["fade_out"] = [](sol::optional<float> sec) { audio_fade_out(sec.value_or(2.0f)); };
    t["set_active_volume"] = [](float gain) { audio_set_active_volume(gain); };
    t["set_volume"] = [](float v) { audio_set_volume(v); };
    t["is_playing"] = []() { return audio_is_playing(); };
    t["is_ready"] = []() { return audio_is_ready(); };
    t["remaining"] = []() { return audio_remaining(); };
    t["get_cursor"] = []() { return audio_get_cursor(); };
    t["at_end"] = []() { return audio_at_end(); };
    t["set_loop_points"] = [](float start_sec, float end_sec) {
        audio_set_loop_points(start_sec, end_sec);
    };
    t["set_intro_loop"] = [](float intro_start, float body_start, float body_end) {
        return audio_set_intro_loop(intro_start, body_start, body_end);
    };
    t["get_length"] = [](const char* path) { return audio_get_length(path ? path : ""); };
    t["seek"] = [](float sec) { audio_seek(sec); };
    t["ansi_to_utf8"] = [](const char* s) { return ansi_to_utf8(s ? s : ""); };
    t["download"] = [](const char* url, const char* outtemplate, sol::optional<bool> playlist,
                       sol::optional<std::string> format) {
        audio_download(url ? url : "", outtemplate ? outtemplate : "", playlist.value_or(false),
                       format.value_or(std::string("mp3")));
    };
    t["download_status"] = []() {
        return g_dl_status.load();
    };
    t["fetch"] = [](const char* url, const char* outpath) {
        audio_fetch(url ? url : "", outpath ? outpath : "");
    };
    t["fetch_status"] = []() {
        return g_fetch_status.load();
    };
}

void on_lua_state_destroyed(lua_State*) {
}

void shutdown_engine() {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& s : g_slots) {
        free_slot(s);
    }
    g_active = -1;
    g_current_path.clear();
    if (g_engine_ok) {
        ma_engine_uninit(&g_engine);
        g_engine_ok = false;
    }
}

}

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    API::initialize(param);
    const auto functions = param->functions;
    functions->on_lua_state_created(on_lua_state_created);
    functions->on_lua_state_destroyed(on_lua_state_destroyed);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        shutdown_engine();
    }
    return TRUE;
}
