// Harissa64 - hi-res texture packs on the console (see xb_hd.h).
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../../core/cart/h64_zip.h"
#include "../../core/common/h64_log.h"
#include "../../core/rdp/h64_hdbuild.h"
#include "xb_hd.h"
#include "xb_ach.h"
#include "xb_net.h"

#define HD_CATALOG_URL "https://raw.githubusercontent.com/kernel64/Harissa64/master/docs/texture-packs.json"
#define HD_WORKERS 4
#define HD_MAX_SIDE 1024
#define HD_FILE_ID_MAX 36       // FATX names: 42 characters, ".h64hd" included

static CRITICAL_SECTION s_lock;
static HANDLE s_thread, s_wake;
static char s_drive[16] = "game";
static char s_catalogUrl[256];
static HdStatus s_st;                       // (s_lock)
static std::vector<H64HdPack> s_cat;        // (s_lock)
static volatile LONG s_catWanted;           // 1: read the catalogue
static volatile LONG s_jobWanted;           // 1: s_job is waiting
static volatile LONG s_stop, s_paused;
static H64HdPack s_job;

// ---- helpers ----

static void set_step(int step, u32 done, u32 total)
{
    EnterCriticalSection(&s_lock);
    s_st.step = step;
    s_st.done = done;
    s_st.total = total;
    LeaveCriticalSection(&s_lock);
}

static void fail(int code, const char *what)
{
    EnterCriticalSection(&s_lock);
    s_st.step = HD_FAILED;
    s_st.errorCode = code;
    strncpy(s_st.error, what, sizeof(s_st.error) - 1);
    s_st.error[sizeof(s_st.error) - 1] = 0;
    LeaveCriticalSection(&s_lock);
    H64_WARN("[hd] %s: %s", s_job.name.c_str(), what);
}

// Waits while a game runs; returns 1 when asked to stop.
static int wait_or_stop(void)
{
    while (s_paused && !s_stop) Sleep(100);
    return s_stop != 0;
}

static int progress(void *user, u32 done, u32 total)
{
    (void)user;
    EnterCriticalSection(&s_lock);
    s_st.done = done;
    s_st.total = total;
    LeaveCriticalSection(&s_lock);
    return wait_or_stop();
}

static u64 file_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static u32 size_mb(u64 n) { return n ? (u32)((n + (1 << 20) - 1) >> 20) : 0; }

// A pack id as a file name: letters, digits, '-' and '_', at most HD_FILE_ID_MAX.
static void file_id(const char *id, char *out, int size)
{
    int n = 0;
    for (; *id && n < size - 1 && n < HD_FILE_ID_MAX; id++)
    {
        char c = *id;
        out[n++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c : '_';
    }
    out[n] = 0;
}

static void pack_path(const char *drive, const char *id, const char *ext, char *out, int size)
{
    char fid[48];
    file_id(id, fid, sizeof(fid));
    _snprintf(out, size - 1, "%s:\\hd_textures\\packs\\%s%s", drive, fid, ext);
    out[size - 1] = 0;
}

static void name_of(const char *folder, char *out, int size)
{
    // "SUPER MARIO 64 NSME" -> "SUPER MARIO 64" (the code is the last word, 4 characters).
    int n;
    strncpy(out, folder, size - 1);
    out[size - 1] = 0;
    n = (int)strlen(out);
    if (n > 5 && out[n - 5] == ' ') out[n - 5] = 0;
}

const char *HdDrive(void) { return s_drive; }

// ---- installed packs ----

// <id>.txt: key=value lines (name, author, version, games, rom, page).
static int write_info(const char *path, const H64HdPack *p)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    fprintf(f, "id=%s\nname=%s\nauthor=%s\nversion=%s\ngames=%s\nrom=%s\npage=%s\n", p->id.c_str(), p->name.c_str(), p->author.c_str(),
            p->version.c_str(), p->games.c_str(), p->rom.c_str(), p->page.c_str());
    ok = !ferror(f);
    fclose(f);
    return ok;
}

static int read_info(const char *path, HdInstalled *p)
{
    char line[512];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '='), *e;
        if (!eq) continue;
        *eq = 0;
        for (e = eq + 1; *e && *e != '\r' && *e != '\n'; e++) ;
        *e = 0;
        if (!strcmp(line, "id")) p->id = eq + 1;
        else if (!strcmp(line, "name")) p->name = eq + 1;
        else if (!strcmp(line, "author")) p->author = eq + 1;
        else if (!strcmp(line, "version")) p->version = eq + 1;
        else if (!strcmp(line, "games")) p->games = eq + 1;
        else if (!strcmp(line, "rom")) p->rom = eq + 1;
    }
    fclose(f);
    return !p->id.empty() && !p->rom.empty();
}

void HdListInstalled(const char *drive, std::vector<HdInstalled> *out)
{
    char pattern[96], path[160];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    out->clear();
    _snprintf(pattern, sizeof(pattern) - 1, "%s:\\hd_textures\\packs\\*.txt", drive);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        HdInstalled p;
        size_t n = strlen(fd.cFileName);
        u64 bytes;
        if (n < 5 || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        _snprintf(path, sizeof(path) - 1, "%s:\\hd_textures\\packs\\%s", drive, fd.cFileName);
        path[sizeof(path) - 1] = 0;
        if (!read_info(path, &p)) continue;
        pack_path(drive, p.id.c_str(), ".h64hd", path, sizeof(path));
        bytes = file_size(path);
        if (!bytes) continue;   // a pack being made, or its pictures removed
        p.mb = size_mb(bytes);
        out->push_back(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int HdInstalledFits(const HdInstalled *p, const char *gameCode, const char *romName)
{
    H64HdPack q;
    q.games = p->games;
    q.rom = p->rom;
    q.size = 0;
    return h64_hdcat_fits_rom(&q, gameCode, romName);
}

int HdOwnFile(const char *drive, const char *folderName, char *path, int size, u32 *mb)
{
    char name[64];
    const char *names[2];
    int i;
    name_of(folderName, name, sizeof(name));
    names[0] = name;
    names[1] = folderName;
    for (i = 0; i < 2; i++)
    {
        u64 n;
        _snprintf(path, size - 1, "%s:\\hd_textures\\%s.h64hd", drive, names[i]);
        path[size - 1] = 0;
        n = file_size(path);
        if (n)
        {
            if (mb) *mb = size_mb(n);
            return 1;
        }
    }
    path[0] = 0;
    if (mb) *mb = 0;
    return 0;
}

int HdResolve(const char *drive, const char *folderName, const char *gameCode, const char *romName, const char *choice,
              char *path, int size, char *id, int idSize)
{
    std::vector<HdInstalled> all;
    size_t i;
    id[0] = 0;
    path[0] = 0;
    if (!choice) choice = "";
    if (choice[0] && strcmp(choice, HD_CHOICE_FILE))
    {
        pack_path(drive, choice, ".h64hd", path, size);
        if (file_size(path))
        {
            strncpy(id, choice, idSize - 1);
            id[idSize - 1] = 0;
            return HD_USE_PACK;
        }
    }
    if (HdOwnFile(drive, folderName, path, size, NULL))
    {
        strncpy(id, HD_CHOICE_FILE, idSize - 1);
        id[idSize - 1] = 0;
        return HD_USE_FILE;
    }
    HdListInstalled(drive, &all);
    for (i = 0; i < all.size(); i++)
        if (HdInstalledFits(&all[i], gameCode, romName))
        {
            pack_path(drive, all[i].id.c_str(), ".h64hd", path, size);
            strncpy(id, all[i].id.c_str(), idSize - 1);
            id[idSize - 1] = 0;
            return HD_USE_PACK;
        }
    path[0] = 0;
    return HD_USE_NONE;
}

int HdRemovePack(const char *id)
{
    char path[160];
    int ok;
    pack_path(s_drive, id, ".h64hd", path, sizeof(path));
    ok = DeleteFileA(path) != 0;
    pack_path(s_drive, id, ".txt", path, sizeof(path));
    DeleteFileA(path);
    if (ok) H64_INFO("[hd] removed pack %s", id);
    return ok;
}

int HdRemoveOwn(const char *folderName)
{
    char path[160];
    if (!HdOwnFile(s_drive, folderName, path, sizeof(path), NULL) || !DeleteFileA(path)) return 0;
    H64_INFO("[hd] removed %s", path);
    return 1;
}

// ---- the catalogue ----

static void read_catalog(void)
{
    char local[160], host[128];
    u8 *body = NULL;
    u32 len = 0;
    int status = 0, n;
    std::vector<H64HdPack> packs;
    _snprintf(local, sizeof(local) - 1, "%s:\\hd_textures\\texture-packs.json", s_drive);
    local[sizeof(local) - 1] = 0;
    {
        FILE *f = fopen(local, "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            len = (u32)ftell(f);
            fseek(f, 0, SEEK_SET);
            body = (u8 *)malloc(len + 1);
            if (body && fread(body, 1, len, f) == len) status = 200;
            fclose(f);
            if (status) H64_INFO("[hd] catalogue: %s", local);
        }
    }
    if (!status)
    {
        const char *u = s_catalogUrl + 8, *slash = strchr(u, '/');
        size_t hl = slash ? (size_t)(slash - u) : 0;
        free(body);
        body = NULL;
        if (strncmp(s_catalogUrl, "https://", 8) || !slash || hl >= sizeof(host) || !NetStart(0)) status = 0;
        else
        {
            memcpy(host, u, hl);
            host[hl] = 0;
            status = NetGet(host, slash, 1, &body, &len, 1 << 20, NULL, 0);
        }
    }
    n = status == 200 && body ? h64_hdcat_parse((const char *)body, len, &packs) : -1;
    free(body);
    EnterCriticalSection(&s_lock);
    if (n >= 0) s_cat = packs;
    s_st.catalogState = n >= 0 ? 2 : 3;
    LeaveCriticalSection(&s_lock);
    if (n >= 0) H64_INFO("[hd] catalogue: %d packs", n);
    else H64_WARN("[hd] catalogue not read (HTTP %d)", status);
}

void HdCatalogRequest(int refresh)
{
    int ask = 0;
    EnterCriticalSection(&s_lock);
    if (s_st.catalogState == 0 || s_st.catalogState == 3 || (refresh && s_st.catalogState == 2))
    {
        s_st.catalogState = 1;
        ask = 1;
    }
    LeaveCriticalSection(&s_lock);
    if (ask && s_thread)
    {
        InterlockedExchange(&s_catWanted, 1);
        SetEvent(s_wake);
    }
}

void HdCatalogFor(const char *gameCode, const char *romName, std::vector<H64HdPack> *out)
{
    size_t i;
    out->clear();
    EnterCriticalSection(&s_lock);
    for (i = 0; i < s_cat.size(); i++)
        if (h64_hdcat_fits_rom(&s_cat[i], gameCode, romName)) out->push_back(s_cat[i]);
    LeaveCriticalSection(&s_lock);
}

// ---- conversion ----

struct ZipSrc { H64ZipEntry entry; H64HdEntry key; };

struct Convert
{
    HANDLE file;
    CRITICAL_SECTION readLock, outLock;
    H64ZipReader reader;
    std::vector<ZipSrc> list;
    H64HdBuilder *builder;
    volatile LONG next, done, writeError, unreadable;
    const char *rom;
};

static u32 zip_read(void *user, u32 offset, void *buf, u32 len)
{
    Convert *c = (Convert *)user;
    DWORD got = 0;
    LONG high = 0;
    EnterCriticalSection(&c->readLock);
    if (SetFilePointer(c->file, (LONG)offset, &high, FILE_BEGIN) != INVALID_SET_FILE_POINTER || GetLastError() == NO_ERROR)
        ReadFile(c->file, buf, len, &got, NULL);
    LeaveCriticalSection(&c->readLock);
    return (u32)got;
}

static int zip_list(void *user, const H64ZipEntry *e)
{
    Convert *c = (Convert *)user;
    const char *slash = strrchr(e->name, '/'), *file = slash ? slash + 1 : e->name;
    size_t n = strlen(file);
    ZipSrc s;
    if (n < 4 || _stricmp(file + n - 4, ".png")) return 0;
    if (h64_hdbuild_skip(e->name) || !h64_hdbuild_parse(file, c->rom, &s.key)) return 0;
    s.entry = *e;
    c->list.push_back(s);
    return 0;
}

static DWORD WINAPI convert_thread(LPVOID arg)
{
    Convert *c = (Convert *)arg;
    std::vector<u8> png;
    for (;;)
    {
        LONG i = InterlockedIncrement(&c->next) - 1;
        H64HdItem item;
        if (i >= (LONG)c->list.size() || c->writeError || wait_or_stop()) break;
        const ZipSrc &s = c->list[i];
        png.resize(s.entry.size ? s.entry.size : 1);
        item.e = s.key;
        item.e.pad2 = (u32)i;   // repeated keys: the first in the pack's order stays
        if (!s.entry.size || h64_zip_extract(&c->reader, &s.entry, &png[0], s.entry.size) ||
            h64_hdbuild_make(&png[0], s.entry.size, HD_MAX_SIDE, &item))
            InterlockedIncrement(&c->unreadable);
        else
        {
            EnterCriticalSection(&c->outLock);
            if (h64_hdbuild_add(c->builder, &item)) c->writeError = 1;
            LeaveCriticalSection(&c->outLock);
        }
        {
            LONG d = InterlockedIncrement(&c->done);
            EnterCriticalSection(&s_lock);
            s_st.done = (u32)d;
            LeaveCriticalSection(&s_lock);
        }
        if (png.size() > (4u << 20)) std::vector<u8>().swap(png);   // (no big buffer kept while a game runs)
    }
    return 0;
}

// Converts the zip into the pack file. Returns HD_DONE, HD_STOPPED or HD_FAILED.
static int convert(const char *zip, const char *tmp, const char *out)
{
    static const DWORD cpus[HD_WORKERS] = { 1, 2, 4, 5 };
    Convert c;
    HANDLE threads[HD_WORKERS];
    int i, started = 0;
    u32 count = 0, repeated = 0, bytes = 0;
    c.file = CreateFileA(zip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (c.file == INVALID_HANDLE_VALUE) { fail(HD_ERR_ZIP, "cannot open the archive"); return HD_FAILED; }
    InitializeCriticalSection(&c.readLock);
    InitializeCriticalSection(&c.outLock);
    c.reader.user = &c;
    c.reader.size = (u32)file_size(zip);
    c.reader.read = zip_read;
    c.next = c.done = c.writeError = c.unreadable = 0;
    c.rom = s_job.rom.c_str();
    c.builder = NULL;
    if (h64_zip_each(&c.reader, zip_list, &c) < 0) { fail(HD_ERR_ZIP, "not a zip archive"); goto out; }
    if (c.list.empty()) { fail(HD_ERR_EMPTY, "no picture for this game in the pack"); goto out; }
    c.builder = h64_hdbuild_begin(tmp, (u32)c.list.size());
    if (!c.builder) { fail(HD_ERR_WRITE, "cannot write the pack file"); goto out; }
    H64_INFO("[hd] converting %u pictures of %s", (u32)c.list.size(), s_job.rom.c_str());
    set_step(HD_CONVERT, 0, (u32)c.list.size());
    for (i = 0; i < HD_WORKERS; i++)
    {
        threads[started] = CreateThread(NULL, 256 * 1024, convert_thread, &c, CREATE_SUSPENDED, NULL);
        if (!threads[started]) continue;
        XSetThreadProcessor(threads[started], cpus[i]);
        ResumeThread(threads[started]);
        started++;
    }
    if (!started) convert_thread(&c);
    for (i = 0; i < started; i++)
    {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
    if (s_stop)
    {
        h64_hdbuild_abort(c.builder);
        c.builder = NULL;
        set_step(HD_STOPPED, 0, 0);
        goto out;
    }
    if (c.writeError)
    {
        h64_hdbuild_abort(c.builder);
        c.builder = NULL;
        fail(HD_ERR_WRITE, "cannot write the pack file (disk full?)");
        goto out;
    }
    if (h64_hdbuild_end(c.builder, &count, &repeated, &bytes))
    {
        c.builder = NULL;
        fail(HD_ERR_WRITE, "cannot finish the pack file");
        goto out;
    }
    c.builder = NULL;
    DeleteFileA(out);
    if (!MoveFileA(tmp, out)) { fail(HD_ERR_WRITE, "cannot rename the pack file"); goto out; }
    H64_INFO("[hd] %s: %u pictures (%u repeated, %d unreadable), %u MB", out, count, repeated, (int)c.unreadable, bytes >> 20);
    set_step(HD_DONE, count, count);
out:
    if (c.builder) h64_hdbuild_abort(c.builder);
    CloseHandle(c.file);
    DeleteCriticalSection(&c.readLock);
    DeleteCriticalSection(&c.outLock);
    {
        int step;
        EnterCriticalSection(&s_lock);
        step = s_st.step;
        LeaveCriticalSection(&s_lock);
        return step;
    }
}

// ---- the job ----

static void run_job(void)
{
    char dir[64], zip[160], tmp[160], out[160], info[160];
    ULARGE_INTEGER freeBytes, total, totalFree;
    u64 have, need;
    int r;
    _snprintf(dir, sizeof(dir) - 1, "%s:\\hd_textures", s_drive);
    dir[sizeof(dir) - 1] = 0;
    CreateDirectoryA(dir, NULL);
    _snprintf(dir, sizeof(dir) - 1, "%s:\\hd_textures\\packs", s_drive);
    dir[sizeof(dir) - 1] = 0;
    CreateDirectoryA(dir, NULL);
    pack_path(s_drive, s_job.id.c_str(), ".zip", zip, sizeof(zip));
    pack_path(s_drive, s_job.id.c_str(), ".tmp", tmp, sizeof(tmp));
    pack_path(s_drive, s_job.id.c_str(), ".h64hd", out, sizeof(out));
    pack_path(s_drive, s_job.id.c_str(), ".txt", info, sizeof(info));
    H64_INFO("[hd] installing %s %s (%s)", s_job.name.c_str(), s_job.version.c_str(), s_job.id.c_str());

    // Room for the rest of the download and the converted pack (about as big).
    have = file_size(zip);
    need = (have < s_job.size ? s_job.size - have : 0) + (u64)s_job.size + (64u << 20);
    if (s_job.size >= 0x7FFFFFFFu) { fail(HD_ERR_SPACE, "packs of 2 GB or more are not supported"); return; }
    {
        char root[16];
        _snprintf(root, sizeof(root) - 1, "%s:\\", s_drive);
        root[sizeof(root) - 1] = 0;
        if (GetDiskFreeSpaceExA(root, &freeBytes, &total, &totalFree) && freeBytes.QuadPart < need)
        {
            fail(HD_ERR_SPACE, "not enough free space");
            return;
        }
    }

    // Download (resumed when an earlier one stopped).
    if (have != s_job.size)
    {
        set_step(HD_DOWNLOAD, (u32)have, s_job.size);
        if (!NetStart(0)) { fail(HD_ERR_NETWORK, "no network"); return; }
        r = NetDownload(s_job.url.c_str(), zip, s_job.size, progress, NULL);
        if (r < 0) { set_step(HD_STOPPED, 0, 0); return; }
        if (r == 0) { fail(HD_ERR_NETWORK, "download failed"); return; }
    }

    // Check.
    if (file_size(zip) != s_job.size)
    {
        DeleteFileA(zip);
        fail(HD_ERR_CHECKSUM, "wrong size");
        return;
    }
    if (!s_job.sha256.empty())
    {
        char hex[65];
        set_step(HD_CHECK, 0, s_job.size);
        r = NetSha256File(zip, hex, progress, NULL);
        if (r < 0) { set_step(HD_STOPPED, 0, 0); return; }
        if (r == 0 || _stricmp(hex, s_job.sha256.c_str()))
        {
            DeleteFileA(zip);
            fail(HD_ERR_CHECKSUM, "the download is damaged (SHA-256)");
            return;
        }
    }

    // The pack's description first (the list shows a pack once both files are there),
    // then the conversion; the archive goes once the pack is made.
    if (!write_info(info, &s_job)) { fail(HD_ERR_WRITE, "cannot write the pack's description"); return; }
    XbAchUnlock(XB_ACH_HD_PACK);
    if (convert(zip, tmp, out) == HD_DONE) DeleteFileA(zip);
    else if (!file_size(out)) DeleteFileA(info);
}

static DWORD WINAPI hd_thread(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(s_wake, INFINITE);
        if (InterlockedExchange(&s_catWanted, 0)) read_catalog();
        if (s_jobWanted)
        {
            s_stop = 0;
            run_job();
            InterlockedExchange(&s_jobWanted, 0);
        }
    }
}

// ---- interface ----

void HdStart(const char *drive, const char *catalogUrl)
{
    if (s_thread) return;
    InitializeCriticalSection(&s_lock);
    memset(&s_st, 0, sizeof(s_st));
    strncpy(s_drive, drive && drive[0] ? drive : "game", sizeof(s_drive) - 1);
    strncpy(s_catalogUrl, catalogUrl && catalogUrl[0] ? catalogUrl : HD_CATALOG_URL, sizeof(s_catalogUrl) - 1);
    s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_thread = CreateThread(NULL, 256 * 1024, hd_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    ResumeThread(s_thread);
}

int HdInstall(const H64HdPack *p)
{
    if (!s_thread || p->id.empty() || InterlockedCompareExchange(&s_jobWanted, 1, 0) != 0) return 0;
    s_job = *p;
    EnterCriticalSection(&s_lock);
    s_st.step = HD_DOWNLOAD;
    s_st.done = 0;
    s_st.total = p->size;
    s_st.errorCode = 0;
    s_st.error[0] = 0;
    strncpy(s_st.id, p->id.c_str(), sizeof(s_st.id) - 1);
    s_st.id[sizeof(s_st.id) - 1] = 0;
    strncpy(s_st.pack, p->name.c_str(), sizeof(s_st.pack) - 1);
    s_st.pack[sizeof(s_st.pack) - 1] = 0;
    LeaveCriticalSection(&s_lock);
    SetEvent(s_wake);
    return 1;
}

void HdStop(void) { InterlockedExchange(&s_stop, 1); }
void HdPause(int paused) { InterlockedExchange(&s_paused, paused ? 1 : 0); }

void HdGetStatus(HdStatus *s)
{
    if (!s_thread) { memset(s, 0, sizeof(*s)); return; }
    EnterCriticalSection(&s_lock);
    *s = s_st;
    LeaveCriticalSection(&s_lock);
    if (s_jobWanted && (s->step == HD_IDLE)) s->step = HD_DOWNLOAD;
}
