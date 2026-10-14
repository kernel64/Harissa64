// Harissa64 - box art and game information for the ROM browser (see xb_art.h).
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../core/cart/h64_rom.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_png.h"
#include "xb_art.h"
#include "xb_menu.h"
#include "xb_net.h"

#define ART_MAX_PNG (4u << 20)          // bytes of a downloaded or cached picture
#define ART_MAX_PIXELS (2048u * 2048u)   // pixels of a picture we decode
#define ART_MAX_DB (4u << 20)
#define ART_MAX_WIKI (256u << 10)
#define ART_MAX_RESULTS 24
#define ART_DB_MAX_AGE_DAYS 30           // "download all" refreshes older databases

// What the worker knows of a ROM file.
struct RomId
{
    int ok;
    char key[16], code[8];
    int version;
    char stem[128];
};

static CRITICAL_SECTION s_lock;
static HANDLE s_event, s_thread;
static char s_dir[64];                       // "game:\art\games"
static volatile LONG s_started, s_netAllowed, s_paused;

// Shared with the UI thread (under s_lock).
static std::vector<std::string> s_wants;
static u32 s_wantGen;
static std::set<std::string> s_delivered;    // ROMs whose result was sent since ArtForgetAll
static std::deque<ArtResult> s_results;
static std::vector<std::string> s_bulk;
static size_t s_bulkPos;
static ArtStatus s_status;
static std::string s_note;

// The worker's own.
static std::set<std::string> s_tried;        // selections whose downloads were tried this session
static std::map<std::string, RomId> s_ids;
static H64GameDb *s_db;

// ---- Files ----
static int file_read(const char *path, u8 **data, u32 *len, u32 maxLen)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    u8 *buf;
    *data = NULL;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > maxLen) { CloseHandle(h); return 0; }
    buf = (u8 *)malloc(size + 1);
    if (!buf || !ReadFile(h, buf, size, &got, NULL) || got != size)
    {
        free(buf);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[size] = 0;
    *data = buf;
    *len = size;
    return 1;
}

static int file_write(const char *path, const void *data, u32 len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, len, &put, NULL) && put == len;
    CloseHandle(h);
    if (!ok) DeleteFileA(path);
    return ok;
}

static int file_exists(const char *path) { return GetFileAttributesA(path) != (DWORD)0xFFFFFFFF; }

// Days since the file was written (a large number when it is missing).
static int file_age_days(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME now;
    ULARGE_INTEGER a, b;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 1 << 20;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (int)((b.QuadPart - a.QuadPart) / (10000000ULL * 86400ULL)) : 0;
}

static void art_path(const char *key, const char *ext, char *out, size_t size)
{
    _snprintf(out, size - 1, "%s\\%s.%s", s_dir, key, ext);
    out[size - 1] = 0;
}

// ---- ROM identity ----
static const RomId *rom_id(const std::string &path)
{
    std::map<std::string, RomId>::iterator it = s_ids.find(path);
    RomId id;
    H64Rom rom;
    const char *base, *dot;
    if (it != s_ids.end()) return &it->second;
    memset(&id, 0, sizeof(id));
    if (RomReadHeader(path.c_str(), &rom))
    {
        id.ok = 1;
        h64_gameinfo_key(rom.gameCode, rom.crc1, id.key);
        strncpy(id.code, rom.gameCode, sizeof(id.code) - 1);
        id.version = rom.version;
        h64_rom_free(&rom);
    }
    base = strrchr(path.c_str(), '\\');
    base = base ? base + 1 : path.c_str();
    dot = strrchr(base, '.');
    _snprintf(id.stem, sizeof(id.stem) - 1, "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
    s_ids[path] = id;
    return &s_ids[path];
}

static int load_info(const RomId *id, H64GameInfo *info)
{
    char path[128];
    u8 *data;
    u32 len;
    h64_gameinfo_clear(info);
    art_path(id->key, "txt", path, sizeof(path));
    if (!file_read(path, &data, &len, 64 * 1024)) return 0;
    {
        int ok = h64_gameinfo_parse(info, (const char *)data, len);
        free(data);
        return ok;
    }
}

static void save_info(const RomId *id, const H64GameInfo *info)
{
    char path[128], text[4096];
    int n = h64_gameinfo_format(info, text, sizeof(text));
    art_path(id->key, "txt", path, sizeof(path));
    if (!file_write(path, text, (u32)n)) H64_WARN("[art] cannot write %s", path);
}

// ---- Databases ----
// Loads the databases from the db folder, downloading those missing (or all
// of them when `refresh`). Returns 1 when the No-Intro list is there.
static int ensure_db(int net, int refresh)
{
    int i, games;
    if (s_db && !refresh) return 1;
    if (s_db) { h64_gamedb_free(s_db); s_db = NULL; }
    s_db = h64_gamedb_new();
    for (i = 0; i < H64_GAMEDB_FILE_COUNT; i++)
    {
        const H64GameDbFile *f = &h64_gamedb_files[i];
        char path[128];
        u8 *data = NULL;
        u32 len = 0;
        int have;
        _snprintf(path, sizeof(path) - 1, "%s\\db\\%s", s_dir, f->file);
        path[sizeof(path) - 1] = 0;
        have = file_exists(path) && !(refresh && file_age_days(path) >= ART_DB_MAX_AGE_DAYS);
        if (have) have = file_read(path, &data, &len, ART_MAX_DB);
        if (!have && net)
        {
            int st = NetGet(f->host, f->path, 1, &data, &len, ART_MAX_DB, NULL, 0);
            if (st == 200 && data)
            {
                file_write(path, data, len);
                have = 1;
                H64_INFO("[art] downloaded %s (%u bytes)", f->file, len);
            }
            else
            {
                free(data);
                data = NULL;
                H64_INFO("[art] %s: download failed (status %d)", f->file, st);
                if (!have) have = file_read(path, &data, &len, ART_MAX_DB);   // an old copy is better than none
            }
        }
        if (!have) continue;
        {
            int n = f->json ? h64_gamedb_add_gamedb_json(s_db, (const char *)data, len) : h64_gamedb_add_dat(s_db, (const char *)data, len);
            if (n < 0) H64_WARN("[art] %s: not readable", f->file);
        }
        free(data);
    }
    games = h64_gamedb_count(s_db);
    H64_INFO("[art] databases: %d games", games);
    if (!games)
    {
        h64_gamedb_free(s_db);
        s_db = NULL;
        return 0;
    }
    return 1;
}

// ---- Downloads ----
// 1: a picture was saved, 0: none of the names has one, -1: the network failed.
static int fetch_cover(const RomId *id, char (*names)[128], int count)
{
    int i, netFail = 0;
    for (i = 0; i < count; i++)
    {
        char thumb[128], enc[400], path[600], file[128];
        u8 *data = NULL;
        u32 len = 0;
        int st, w, h;
        h64_gameinfo_thumb_name(names[i], thumb, sizeof(thumb));
        h64_url_encode(thumb, enc, sizeof(enc));
        _snprintf(path, sizeof(path) - 1, "%s%s.png", H64_THUMB_PATH, enc);
        path[sizeof(path) - 1] = 0;
        st = NetGet(H64_THUMB_HOST, path, 1, &data, &len, ART_MAX_PNG, NULL, 0);
        if (st == 200 && data && len < 256 && data[0] != 0x89 && len > 4 && !memcmp(data + len - 4, ".png", 4) && !strchr((char *)data, '/'))
        {
            // A git symbolic link (libretro-thumbnails names aliases this way): its text is the file it points to.
            h64_url_encode((const char *)data, enc, sizeof(enc));
            free(data);
            data = NULL;
            _snprintf(path, sizeof(path) - 1, "%s%s", H64_THUMB_PATH, enc);
            path[sizeof(path) - 1] = 0;
            st = NetGet(H64_THUMB_HOST, path, 1, &data, &len, ART_MAX_PNG, NULL, 0);
        }
        if (st == 200 && data)
        {
            u8 *px = h64_png_decode(data, len, &w, &h, ART_MAX_PIXELS);
            if (px)
            {
                free(px);
                art_path(id->key, "png", file, sizeof(file));
                if (file_write(file, data, len))
                {
                    H64_INFO("[art] %s: box art \"%s\" (%dx%d, %u bytes)", id->key, names[i], w, h, len);
                    free(data);
                    return 1;
                }
            }
            else
                H64_INFO("[art] %s: \"%s\" is not a usable PNG", id->key, names[i]);
        }
        else if (!st)
            netFail = 1;
        free(data);
    }
    return netFail ? -1 : 0;
}

static int fetch_wiki(H64GameInfo *info)
{
    char pages[3][256];
    int n = h64_gameinfo_wiki_pages(info, pages, 3), i, netFail = 0;
    for (i = 0; i < n; i++)
    {
        char path[400];
        u8 *data = NULL;
        u32 len = 0;
        int st;
        _snprintf(path, sizeof(path) - 1, "%s%s", H64_WIKI_PATH, pages[i]);
        path[sizeof(path) - 1] = 0;
        st = NetGet(H64_WIKI_HOST, path, 1, &data, &len, ART_MAX_WIKI, NULL, 0);
        if (st == 200 && data && h64_gameinfo_wiki_extract((const char *)data, len, info->description, sizeof(info->description)))
        {
            free(data);
            return 1;
        }
        if (!st) netFail = 1;
        free(data);
        if (netFail) break;
    }
    return netFail ? -1 : 0;
}

static void set_busy(const std::string &path);

// Downloads what a ROM is missing. `bulk`: also retries what was not found
// before. Returns 1 when its files changed.
static int download_missing(const std::string &path, int bulk)
{
    const RomId *id = rom_id(path);
    H64GameInfo info;
    char png[128], covers[8][128];
    int have, hasPng, nc = 0, changed = 0;
    if (!id->ok) return 0;
    have = load_info(id, &info);
    art_path(id->key, "png", png, sizeof(png));
    hasPng = file_exists(png);
    // What is known to be missing is only retried by "download all".
    if (have && !info.found && !bulk) return 0;
    if (have && info.found && (hasPng || (info.cover == 0 && !bulk)) && info.wiki >= 0) return 0;
    // (The first call waits for the network; "download all" retried it when it started.)
    if (!NetStart(0))
    {
        s_status.offline = 1;
        return 0;
    }
    s_status.offline = 0;
    set_busy(path);   // the browser shows "Downloading" from here
    if (ensure_db(1, 0))
    {
        H64GameInfo fresh;
        h64_gameinfo_clear(&fresh);
        h64_gamedb_lookup(s_db, id->code, id->version, id->stem, &fresh, covers, 8, &nc);
        if (!have || !info.found)
        {
            // First identification (or a retry): what the databases say.
            if (have && info.wiki > 0) { strcpy(fresh.description, info.description); fresh.wiki = 1; }
            fresh.cover = hasPng ? 1 : -1;
            info = fresh;
            have = 1;
            changed = 1;
            save_info(id, &info);
            H64_INFO("[art] %s (%s): %s", id->key, id->stem, info.found ? info.name : "not in the databases");
        }
    }
    if (!have) return changed;
    if (info.found && !hasPng && (info.cover != 0 || bulk))
    {
        int r;
        if (!nc && info.name[0]) { strcpy(covers[0], info.name); nc = 1; }
        r = fetch_cover(id, covers, nc);
        if (r >= 0 || info.cover != r)
        {
            info.cover = r > 0 ? 1 : r == 0 ? 0 : -1;
            save_info(id, &info);
            changed = 1;
        }
    }
    if (info.found && info.wiki < 0)
    {
        int r = fetch_wiki(&info);
        if (r >= 0)
        {
            info.wiki = r;
            save_info(id, &info);
            changed = 1;
        }
    }
    return changed;
}

// ---- Results ----
static void result_free(ArtResult *r)
{
    free(r->cover);
    free(r->glow);
    r->cover = r->glow = NULL;
}

// Reads a ROM's files and queues them for the UI.
static void deliver(const std::string &path)
{
    const RomId *id = rom_id(path);
    ArtResult r;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, path.c_str(), sizeof(r.path) - 1);
    if (id->ok)
    {
        char png[128];
        u8 *data;
        u32 len;
        strcpy(r.key, id->key);
        r.hasInfo = load_info(id, &r.info);
        art_path(id->key, "png", png, sizeof(png));
        if (file_read(png, &data, &len, ART_MAX_PNG))
        {
            int w, h;
            u8 *px = h64_png_decode(data, len, &w, &h, ART_MAX_PIXELS);
            free(data);
            if (px)
            {
                h64_image_fit(w, h, ART_COVER_W, ART_COVER_H, &r.coverW, &r.coverH);
                r.cover = h64_image_scale(px, w, h, r.coverW, r.coverH);
                r.glow = h64_image_scale(px, w, h, ART_GLOW_W, ART_GLOW_H);
                free(px);
            }
            else
                H64_INFO("[art] %s: cannot decode %s", id->key, png);
        }
    }
    else
        h64_gameinfo_clear(&r.info);
    EnterCriticalSection(&s_lock);
    while (s_results.size() >= ART_MAX_RESULTS)
    {
        s_delivered.erase(s_results.front().path);   // never reached the browser: sent again when wanted
        result_free(&s_results.front());
        s_results.pop_front();
    }
    s_results.push_back(r);
    s_delivered.insert(path);
    LeaveCriticalSection(&s_lock);
}

// ---- Worker ----
static void set_busy(const std::string &path)
{
    EnterCriticalSection(&s_lock);
    strncpy(s_status.busyPath, path.c_str(), sizeof(s_status.busyPath) - 1);
    s_status.busyPath[sizeof(s_status.busyPath) - 1] = 0;
    LeaveCriticalSection(&s_lock);
}

static void note(const char *text)
{
    EnterCriticalSection(&s_lock);
    s_note = text;
    LeaveCriticalSection(&s_lock);
}

// One step of the wanted list; returns 1 when it did something.
static int step_wants(void)
{
    std::vector<std::string> wants;
    u32 gen;
    size_t i;
    EnterCriticalSection(&s_lock);
    wants = s_wants;
    gen = s_wantGen;
    LeaveCriticalSection(&s_lock);
    for (i = 0; i < wants.size(); i++)
    {
        int sent, tried;
        EnterCriticalSection(&s_lock);
        sent = s_delivered.count(wants[i]) != 0;
        LeaveCriticalSection(&s_lock);
        tried = s_tried.count(wants[i]) != 0;
        if (!sent)
        {
            deliver(wants[i]);   // first what is there (fast), the downloads next
            return 1;
        }
        if (i == 0 && !tried && s_netAllowed)
        {
            int changed;
            s_tried.insert(wants[0]);
            changed = download_missing(wants[0], 0);
            set_busy("");
            if (changed)
            {
                EnterCriticalSection(&s_lock);
                if (s_wantGen == gen || std::find(s_wants.begin(), s_wants.end(), wants[0]) != s_wants.end()) s_delivered.erase(wants[0]);
                LeaveCriticalSection(&s_lock);
            }
            return 1;
        }
    }
    return 0;
}

static int step_bulk(void)
{
    std::string path;
    size_t pos, total;
    EnterCriticalSection(&s_lock);
    if (!s_status.bulk || s_bulkPos >= s_bulk.size())
    {
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    pos = s_bulkPos;
    total = s_bulk.size();
    path = s_bulk[pos];
    LeaveCriticalSection(&s_lock);

    if (pos == 0)
    {
        // Fresh databases first; no network: stop here.
        if (!NetStart(1))
        {
            char text[96];
            EnterCriticalSection(&s_lock);
            s_status.bulk = 0;
            s_status.offline = 1;
            s_bulk.clear();
            LeaveCriticalSection(&s_lock);
            strcpy(text, "No network connection: nothing was downloaded");
            note(text);
            return 1;
        }
        ensure_db(1, 1);
    }
    if (download_missing(path, 1))
    {
        EnterCriticalSection(&s_lock);
        s_delivered.erase(path);   // shown again if the browser still wants it
        LeaveCriticalSection(&s_lock);
    }
    set_busy("");
    {
        const RomId *id = rom_id(path);
        H64GameInfo info;
        char png[128];
        int found = 0, cover = 0;
        if (id->ok && load_info(id, &info)) found = info.found;
        if (id->ok)
        {
            art_path(id->key, "png", png, sizeof(png));
            cover = file_exists(png);
        }
        EnterCriticalSection(&s_lock);
        s_status.done = (int)pos + 1;
        s_status.found += found;
        s_status.covers += cover;
        s_bulkPos = pos + 1;
        if (s_bulkPos >= total)
        {
            char text[128];
            s_status.bulk = 0;
            s_bulk.clear();
            _snprintf(text, sizeof(text) - 1, "Box art: %d of %d games have a picture%s", s_status.covers, (int)total,
                      s_status.offline ? " (connection lost)" : "");
            text[sizeof(text) - 1] = 0;
            s_note = text;
            H64_INFO("[art] download all: %d games, %d identified, %d with box art", (int)total, s_status.found, s_status.covers);
        }
        LeaveCriticalSection(&s_lock);
    }
    return 1;
}

static DWORD WINAPI art_thread(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(s_event, 100);
        while (!s_paused && (step_wants() || step_bulk())) ;
    }
}

// ---- API ----
void ArtStart(const char *drive)
{
    char dir[96];
    if (InterlockedExchange(&s_started, 1)) return;
    InitializeCriticalSection(&s_lock);
    s_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    _snprintf(dir, sizeof(dir) - 1, "%s:\\art", drive);
    dir[sizeof(dir) - 1] = 0;
    CreateDirectoryA(dir, NULL);
    _snprintf(s_dir, sizeof(s_dir) - 1, "%s:\\art\\games", drive);
    CreateDirectoryA(s_dir, NULL);
    _snprintf(dir, sizeof(dir) - 1, "%s\\db", s_dir);
    CreateDirectoryA(dir, NULL);
    // A big stack for the TLS handshakes; hardware thread 3 (as the update
    // check), away from the CPU (0), graphics (2, 5) and audio (4) threads.
    s_thread = CreateThread(NULL, 256 * 1024, art_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(s_thread);
    H64_INFO("[art] folder %s", s_dir);
}

void ArtSetNetwork(int allowed) { InterlockedExchange(&s_netAllowed, allowed ? 1 : 0); }

void ArtPause(int paused)
{
    InterlockedExchange(&s_paused, paused ? 1 : 0);
    if (!paused && s_event) SetEvent(s_event);
}

void ArtWant(const char *const *paths, int count)
{
    std::vector<std::string> w;
    int i;
    if (!s_started) return;
    for (i = 0; i < count && i < 8; i++) w.push_back(paths[i]);
    EnterCriticalSection(&s_lock);
    if (w != s_wants)
    {
        s_wants = w;
        s_wantGen++;
        SetEvent(s_event);
    }
    LeaveCriticalSection(&s_lock);
}

int ArtTake(ArtResult *r)
{
    int got = 0;
    if (!s_started) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_results.empty())
    {
        *r = s_results.front();
        s_results.pop_front();
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}

void ArtResultFree(ArtResult *r) { result_free(r); }

void ArtForget(const char *path)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    s_delivered.erase(path);
    SetEvent(s_event);
    LeaveCriticalSection(&s_lock);
}

void ArtForgetAll(void)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    s_delivered.clear();
    while (!s_results.empty())
    {
        result_free(&s_results.front());
        s_results.pop_front();
    }
    LeaveCriticalSection(&s_lock);
}

void ArtBulkStart(const char *const *paths, int count)
{
    int i;
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    if (!s_status.bulk && count > 0)
    {
        s_bulk.clear();
        for (i = 0; i < count; i++) s_bulk.push_back(paths[i]);
        s_bulkPos = 0;
        s_status.bulk = 1;
        s_status.done = s_status.found = s_status.covers = 0;
        s_status.total = count;
        SetEvent(s_event);
    }
    LeaveCriticalSection(&s_lock);
}

void ArtGetStatus(ArtStatus *s)
{
    if (!s_started) { memset(s, 0, sizeof(*s)); return; }
    EnterCriticalSection(&s_lock);
    *s = s_status;
    LeaveCriticalSection(&s_lock);
}

int ArtTakeNote(char *text, int size)
{
    int got = 0;
    if (!s_started || size <= 0) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_note.empty())
    {
        strncpy(text, s_note.c_str(), size - 1);
        text[size - 1] = 0;
        s_note.clear();
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}
