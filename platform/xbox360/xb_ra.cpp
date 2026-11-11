// Harissa64 - RetroAchievements on the console (see xb_ra.h).
#include "xb_ra.h"

#include <xtl.h>
#include <xbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_client.h"

#include "../../core/cart/h64_rom.h"
#include "../../core/cheat/h64_cheat.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_types.h"
#include "../../core/ra/h64_ra.h"
#include "../../core/system/h64_system.h"
#include "xb_ach.h"
#include "xb_config.h"
#include "xb_lang.h"
#include "xb_net.h"

// ---- Server calls ----
// rc_client asks for a call and gets the answer later: a worker thread does
// the HTTPS request, XbRaPump (the menus' and the game's thread) hands the
// answer over, so rc_client only ever runs on one thread.
struct RaCall
{
    char *url, *post, *type;
    rc_client_server_callback_t callback;
    void *data;
    u8 *body;
    u32 len;
    int status;
    RaCall *next;
};
static RaCall *s_todo, *s_todoTail, *s_done, *s_doneTail;
static CRITICAL_SECTION s_qLock, s_pumpLock;
static HANDLE s_wake, s_thread;

static rc_client_t *s_client;
static int s_enabled, s_state = XB_RA_OFF;
static char s_drive[16], s_iniPath[96], s_logPath[96];
static char s_user[64], s_token[96];
static void (*s_toast)(const char *text, int ms);
static H64System *s_sys;
static int s_gameAsked, s_cheated;
static char s_code[4];   // the running game's code without the region ("NSM")

static char *copy_text(const char *s)
{
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void call_run(RaCall *c)
{
    char host[128], agent[96];
    const char *p = c->url, *slash;
    size_t hl;
    c->status = 0;
    if (!p || strncmp(p, "https://", 8)) return;
    p += 8;
    slash = strchr(p, '/');
    hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl >= sizeof(host)) return;
    memcpy(host, p, hl);
    host[hl] = 0;
    if (!NetStart(0)) return;
    agent[0] = 0;
    rc_client_get_user_agent_clause(s_client, agent, sizeof(agent));
    if (c->post) c->status = NetPost(host, slash ? slash : "/", c->post, c->type, agent, &c->body, &c->len, 8 << 20);
    else c->status = NetGet(host, slash ? slash : "/", 1, &c->body, &c->len, 8 << 20, NULL, 0);
}

static DWORD WINAPI ra_thread(LPVOID)
{
    for (;;)
    {
        RaCall *c;
        WaitForSingleObject(s_wake, INFINITE);
        for (;;)
        {
            EnterCriticalSection(&s_qLock);
            c = s_todo;
            if (c)
            {
                s_todo = c->next;
                if (!s_todo) s_todoTail = NULL;
            }
            LeaveCriticalSection(&s_qLock);
            if (!c) break;
            call_run(c);
            c->next = NULL;
            EnterCriticalSection(&s_qLock);
            if (s_doneTail) s_doneTail->next = c;
            else s_done = c;
            s_doneTail = c;
            LeaveCriticalSection(&s_qLock);
        }
    }
}

static void RC_CCONV server_call(const rc_api_request_t *request, rc_client_server_callback_t callback, void *data, rc_client_t *)
{
    RaCall *c = (RaCall *)calloc(1, sizeof(RaCall));
    if (!c)
    {
        rc_api_server_response_t r;
        memset(&r, 0, sizeof(r));
        r.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        callback(&r, data);
        return;
    }
    c->url = copy_text(request->url);
    c->post = copy_text(request->post_data);
    c->type = copy_text(request->content_type);
    c->callback = callback;
    c->data = data;
    EnterCriticalSection(&s_qLock);
    if (s_todoTail) s_todoTail->next = c;
    else s_todo = c;
    s_todoTail = c;
    LeaveCriticalSection(&s_qLock);
    SetEvent(s_wake);
}

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t count, rc_client_t *)
{
    return s_sys ? h64_ra_read(s_sys, address, buffer, count) : 0;
}

// The console's GetTickCount falls behind while a game runs: QPC.
static rc_clock_t RC_CCONV now_ms(const rc_client_t *)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (rc_clock_t)(t.QuadPart * 1000 / freq.QuadPart);
}

static void RC_CCONV log_message(const char *message, const rc_client_t *)
{
    H64_INFO("[ra] %s", message);
}

static void toast(const char *text, int ms)
{
    if (s_toast) s_toast(text, ms);
}

// ---- What was unlocked here, and the Xbox achievements it gives ----
// <drive>:\config\ra_unlocks.txt: "A game achievement points code" per
// achievement unlocked on this console, "M game" per game completed.
#define RA_MAX_UNLOCKS 8192
#define RA_MAX_GAMES 512
static u32 s_unlockId[RA_MAX_UNLOCKS];
static u32 s_gameId[RA_MAX_GAMES], s_masterId[RA_MAX_GAMES];
static int s_unlocks, s_games, s_masters, s_points;
static int s_named[5];   // achievements in SM64, OoT, MK64, GoldenEye, Banjo-Kazooie

static int named_index(const char *code)
{
    static const char *const codes[6] = { "NSM", "CZL", "NKT", "NGE", "NBK", "NZL" };
    int i;
    for (i = 0; i < 6; i++)
        if (!strncmp(code, codes[i], 3)) return i == 5 ? 1 : i;
    return -1;
}

static int unlock_count(u32 game, u32 id, u32 points, const char *code)
{
    int i, n;
    for (i = 0; i < s_unlocks; i++)
        if (s_unlockId[i] == id) return 0;
    if (s_unlocks >= RA_MAX_UNLOCKS) return 0;
    s_unlockId[s_unlocks++] = id;
    s_points += (int)points;
    for (i = 0; i < s_games; i++)
        if (s_gameId[i] == game) break;
    if (i == s_games && s_games < RA_MAX_GAMES) s_gameId[s_games++] = game;
    n = named_index(code);
    if (n >= 0) s_named[n]++;
    return 1;
}

static int master_count(u32 game)
{
    int i;
    for (i = 0; i < s_masters; i++)
        if (s_masterId[i] == game) return 0;
    if (s_masters >= RA_MAX_GAMES) return 0;
    s_masterId[s_masters++] = game;
    return 1;
}

static void xbox_check(void)
{
    static const int named[5] = { XB_ACH_SM64, XB_ACH_OOT, XB_ACH_MK64, XB_ACH_GE007, XB_ACH_BANJO };
    int i;
    if (s_unlocks >= 1) XbAchUnlock(XB_ACH_FIRST);
    if (s_unlocks >= 10) XbAchUnlock(XB_ACH_COLLECT_10);
    if (s_unlocks >= 50) XbAchUnlock(XB_ACH_COLLECT_50);
    if (s_unlocks >= 100) XbAchUnlock(XB_ACH_COLLECT_100);
    if (s_unlocks >= 250) XbAchUnlock(XB_ACH_COLLECT_250);
    if (s_unlocks >= 500) XbAchUnlock(XB_ACH_COLLECT_500);
    if (s_points >= 100) XbAchUnlock(XB_ACH_POINTS_100);
    if (s_points >= 1000) XbAchUnlock(XB_ACH_POINTS_1000);
    if (s_points >= 5000) XbAchUnlock(XB_ACH_POINTS_5000);
    if (s_games >= 3) XbAchUnlock(XB_ACH_GAMES_3);
    if (s_games >= 10) XbAchUnlock(XB_ACH_GAMES_10);
    if (s_games >= 25) XbAchUnlock(XB_ACH_GAMES_25);
    if (s_masters >= 1) XbAchUnlock(XB_ACH_MASTER_1);
    if (s_masters >= 3) XbAchUnlock(XB_ACH_MASTER_3);
    for (i = 0; i < 5; i++)
        if (s_named[i] >= 10) XbAchUnlock(named[i]);
}

static void unlocks_load(void)
{
    char line[128];
    FILE *f = fopen(s_logPath, "rb");
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        unsigned game = 0, id = 0, points = 0;
        char code[8];
        code[0] = 0;
        if (line[0] == 'A' && sscanf(line + 1, "%u %u %u %7s", &game, &id, &points, code) >= 3) unlock_count(game, id, points, code);
        else if (line[0] == 'M' && sscanf(line + 1, "%u", &game) == 1) master_count(game);
    }
    fclose(f);
    H64_INFO("[ra] unlocked on this console: %d achievements, %d points, %d games, %d completed", s_unlocks, s_points, s_games, s_masters);
}

static void unlocks_append(const char *line)
{
    // "ab" is not trusted to append on the console: seek to the end.
    FILE *f = fopen(s_logPath, "r+b");
    if (!f) f = fopen(s_logPath, "wb");
    if (!f) { H64_WARN("[ra] cannot write %s", s_logPath); return; }
    fseek(f, 0, SEEK_END);
    fputs(line, f);
    fclose(f);
}

// ---- Events ----
static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *client)
{
    char text[200], line[96];
    const rc_client_game_t *game = rc_client_get_game_info(client);
    u32 gameId = game ? game->id : 0;
    switch (e->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        _snprintf(text, sizeof(text) - 1, Tr(T_RA_UNLOCKED), e->achievement->title, (int)e->achievement->points);
        text[sizeof(text) - 1] = 0;
        toast(text, 6000);
        H64_INFO("[ra] unlocked: %u \"%s\" (%u points)", e->achievement->id, e->achievement->title, e->achievement->points);
        if (unlock_count(gameId, e->achievement->id, e->achievement->points, s_code))
        {
            _snprintf(line, sizeof(line) - 1, "A %u %u %u %s\r\n", gameId, e->achievement->id, e->achievement->points, s_code[0] ? s_code : "-");
            line[sizeof(line) - 1] = 0;
            unlocks_append(line);
        }
        xbox_check();
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        toast(Tr(T_RA_MASTERED), 6000);
        H64_INFO("[ra] game %u completed", gameId);
        if (master_count(gameId))
        {
            _snprintf(line, sizeof(line) - 1, "M %u\r\n", gameId);
            line[sizeof(line) - 1] = 0;
            unlocks_append(line);
        }
        xbox_check();
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        H64_WARN("[ra] server error (%s): %s", e->server_error->api, e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        H64_WARN("[ra] the server cannot be reached: unlocks are kept and sent later");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        H64_INFO("[ra] the server is back: pending unlocks sent");
        break;
    default:
        break;
    }
}

// ---- Account ----
static void ini_read(void)
{
    char line[200];
    FILE *f = fopen(s_iniPath, "rb");
    s_user[0] = s_token[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
        if (!strncmp(line, "user=", 5)) { strncpy(s_user, line + 5, sizeof(s_user) - 1); s_user[sizeof(s_user) - 1] = 0; }
        else if (!strncmp(line, "token=", 6)) { strncpy(s_token, line + 6, sizeof(s_token) - 1); s_token[sizeof(s_token) - 1] = 0; }
    }
    fclose(f);
}

static void ini_write(void)
{
    if (!ConfigWriteKey(s_iniPath, "user", s_user) || !ConfigWriteKey(s_iniPath, "token", s_token)) H64_WARN("[ra] cannot write %s", s_iniPath);
}

static void RC_CCONV on_game(int result, const char *error, rc_client_t *client, void *)
{
    char text[160];
    rc_client_user_game_summary_t sum;
    const rc_client_game_t *game = rc_client_get_game_info(client);
    if (result != RC_OK)
    {
        H64_WARN("[ra] game not loaded (%d): %s", result, error ? error : "");
        if (result == RC_NO_GAME_LOADED) toast(Tr(T_RA_NONE), 4000);
        return;
    }
    rc_client_get_user_game_summary(client, &sum);
    H64_INFO("[ra] game %u \"%s\": %u achievements, %u unlocked, %u not supported", game ? game->id : 0, game ? game->title : "",
             sum.num_promoted_achievements, sum.num_unlocked_achievements, sum.num_unsupported_achievements);
    if (!sum.num_promoted_achievements) { toast(Tr(T_RA_NONE), 4000); return; }
    _snprintf(text, sizeof(text) - 1, Tr(T_RA_GAME), (int)sum.num_unlocked_achievements, (int)sum.num_promoted_achievements);
    text[sizeof(text) - 1] = 0;
    toast(text, 5000);
    xbox_check();
}

static void game_load(void)
{
    char hash[33];
    if (!s_client || !s_enabled || !s_sys || s_gameAsked || s_state == XB_RA_OUT) return;
    s_gameAsked = 1;
    h64_ra_rom_hash(&s_sys->rom, hash);
    H64_INFO("[ra] game hash %s", hash);
    rc_client_begin_load_game(s_client, hash, on_game, NULL);
}

static void RC_CCONV on_login(int result, const char *error, rc_client_t *client, void *)
{
    char text[160];
    const rc_client_user_t *u = result == RC_OK ? rc_client_get_user_info(client) : NULL;
    if (!u)
    {
        H64_WARN("[ra] connection failed (%d): %s", result, error ? error : "");
        s_state = XB_RA_OUT;
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)
        {
            s_token[0] = 0;   // the token is dead: the player connects again
            ini_write();
        }
        toast(Tr(T_RA_LOGIN_FAILED), 4000);
        return;
    }
    strncpy(s_user, u->username, sizeof(s_user) - 1);
    s_user[sizeof(s_user) - 1] = 0;
    strncpy(s_token, u->token, sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = 0;
    ini_write();
    s_state = XB_RA_IN;
    H64_INFO("[ra] connected as %s", u->display_name);
    _snprintf(text, sizeof(text) - 1, Tr(T_RA_CONNECTED), u->display_name);
    text[sizeof(text) - 1] = 0;
    toast(text, 4000);
    game_load();
}

static void token_login(void)
{
    if (!s_client || !s_enabled || s_state != XB_RA_OUT || !s_user[0] || !s_token[0]) return;
    s_state = XB_RA_BUSY;
    rc_client_begin_login_with_token(s_client, s_user, s_token, on_login, NULL);
}

static void client_make(void)
{
    if (s_client) return;
    InitializeCriticalSection(&s_qLock);
    InitializeCriticalSection(&s_pumpLock);
    s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_client = rc_client_create(read_memory, server_call);
    if (!s_client) return;
    rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_get_time_millisecs_function(s_client, now_ms);
    rc_client_set_event_handler(s_client, on_event);
    rc_client_set_hardcore_enabled(s_client, 0);   // softcore: save states are allowed
    s_thread = CreateThread(NULL, 256 * 1024, ra_thread, NULL, CREATE_SUSPENDED, NULL);
    if (s_thread)
    {
        XSetThreadProcessor(s_thread, 3);
        ResumeThread(s_thread);
    }
    unlocks_load();
}

void XbRaInit(const char *drive, int enabled, void (*toastFn)(const char *text, int ms))
{
    strncpy(s_drive, drive, sizeof(s_drive) - 1);
    _snprintf(s_iniPath, sizeof(s_iniPath) - 1, "%s:\\config\\ra.ini", s_drive);
    _snprintf(s_logPath, sizeof(s_logPath) - 1, "%s:\\config\\ra_unlocks.txt", s_drive);
    s_toast = toastFn;
    XbRaSetEnabled(enabled);
}

void XbRaSetEnabled(int on)
{
    on = on != 0;
    if (on == s_enabled) return;
    if (on)
    {
        client_make();
        if (!s_client) return;
        s_enabled = 1;
        if (s_state == XB_RA_OFF)
        {
            s_state = XB_RA_OUT;
            ini_read();
        }
        token_login();
        if (s_sys) XbRaGameStart(s_sys);
    }
    else
    {
        // The client stays (answers on their way still have somewhere to go).
        H64System *sys = s_sys;
        if (sys) XbRaGameEnd(sys);
        s_sys = sys;   // remembered: turned on again, the game's achievements load
        s_enabled = 0;
    }
}

int XbRaState(void) { return s_enabled ? s_state : XB_RA_OFF; }
const char *XbRaUser(void) { return s_enabled && s_state == XB_RA_IN ? s_user : ""; }

// ---- The Xbox keyboard: name, then password ----
static int s_kb;   // 0: closed, 1: the name, 2: the password, 3: the password, once the name's keyboard is gone
static DWORD s_kbSince;   // when the wait of 3 began
static WCHAR s_kbText[128], s_kbTitle[96], s_kbDesc[160];
static XOVERLAPPED s_kbOv;
static char s_kbUser[64];

static void kb_open(int which)
{
    DWORD r;
    memset(&s_kbOv, 0, sizeof(s_kbOv));
    memset(s_kbText, 0, sizeof(s_kbText));
    MultiByteToWideChar(CP_UTF8, 0, Tr(which == 1 ? T_RA_USER_TITLE : T_RA_PASS_TITLE), -1, s_kbTitle, 95);
    MultiByteToWideChar(CP_UTF8, 0, Tr(T_RA_KB_DESC), -1, s_kbDesc, 159);
    r = XShowKeyboardUI(0, which == 1 ? VKBD_DEFAULT : VKBD_LATIN_PASSWORD, L"", s_kbTitle, s_kbDesc, s_kbText, 127, &s_kbOv);
    if (r == ERROR_IO_PENDING) { s_kb = which; return; }
    // Right after the name, the system still has its keyboard up and refuses
    // another (error 5, seen on the console): asked again for a few seconds.
    if (which == 2 && r == ERROR_ACCESS_DENIED && (s_kb != 3 || GetTickCount() - s_kbSince < 8000))
    {
        if (s_kb != 3) s_kbSince = GetTickCount();
        s_kb = 3;
        return;
    }
    s_kb = 0;
    H64_WARN("[ra] the keyboard did not open (%lu)", r);
}

static void kb_pump(void)
{
    DWORD r;
    char text[256];
    if (s_kb == 3) { kb_open(2); return; }
    if (!s_kb || !XHasOverlappedIoCompleted(&s_kbOv)) return;
    r = XGetOverlappedResult(&s_kbOv, NULL, TRUE);
    text[0] = 0;
    if (r == ERROR_SUCCESS) WideCharToMultiByte(CP_UTF8, 0, s_kbText, -1, text, sizeof(text) - 1, NULL, NULL);
    text[sizeof(text) - 1] = 0;
    memset(s_kbText, 0, sizeof(s_kbText));
    if (!text[0]) { s_kb = 0; return; }   // cancelled
    if (s_kb == 1)
    {
        strncpy(s_kbUser, text, sizeof(s_kbUser) - 1);
        s_kbUser[sizeof(s_kbUser) - 1] = 0;
        kb_open(2);
        return;
    }
    s_kb = 0;
    s_state = XB_RA_BUSY;
    H64_INFO("[ra] connecting as %s", s_kbUser);
    rc_client_begin_login_with_password(s_client, s_kbUser, text, on_login, NULL);
    memset(text, 0, sizeof(text));   // the password is not kept
}

void XbRaAccount(void)
{
    if (!s_client || !s_enabled || s_kb || s_state == XB_RA_BUSY) return;
    if (s_state == XB_RA_IN)
    {
        if (s_sys) rc_client_unload_game(s_client);
        rc_client_logout(s_client);
        s_gameAsked = 0;
        s_user[0] = s_token[0] = 0;
        ini_write();
        s_state = XB_RA_OUT;
        H64_INFO("[ra] disconnected");
        toast(Tr(T_RA_LOGGED_OUT), 3000);
        return;
    }
    kb_open(1);
}

int XbRaBusy(void) { return s_kb != 0; }

void XbRaPump(void)
{
    if (!s_client) return;
    EnterCriticalSection(&s_pumpLock);
    kb_pump();
    for (;;)
    {
        RaCall *c;
        rc_api_server_response_t r;
        EnterCriticalSection(&s_qLock);
        c = s_done;
        if (c)
        {
            s_done = c->next;
            if (!s_done) s_doneTail = NULL;
        }
        LeaveCriticalSection(&s_qLock);
        if (!c) break;
        memset(&r, 0, sizeof(r));
        r.body = (const char *)c->body;
        r.body_length = c->len;
        r.http_status_code = c->status ? c->status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        c->callback(&r, c->data);
        free(c->body);
        free(c->url);
        free(c->post);
        free(c->type);
        free(c);
    }
    LeaveCriticalSection(&s_pumpLock);
}

// ---- The game ----
static void vi_hook(H64System *sys)
{
    XbRaPump();
    if (!s_cheated && sys->cheats && h64_cheat_active(sys->cheats))
    {
        s_cheated = 1;
        H64_INFO("[ra] a cheat is on: achievements paused until the game restarts");
        toast(Tr(T_RA_CHEATS_OFF), 5000);
    }
    if (s_cheated) rc_client_idle(s_client);
    else rc_client_do_frame(s_client);
    // What the site shows of the game in progress, in the log when it changes
    // (looked at every 5 s or so): it is computed from the game's memory, so a
    // sensible text says the achievements read the right bytes.
    if (sys->vi.frames % 300 == 0)
    {
        static char s_presence[256];
        char now[256];
        now[0] = 0;
        rc_client_get_rich_presence_message(s_client, now, sizeof(now));
        if (strcmp(now, s_presence))
        {
            strcpy(s_presence, now);
            H64_INFO("[ra] presence: %s", now);
        }
    }
}

void XbRaGameStart(H64System *sys)
{
    s_sys = sys;
    s_gameAsked = 0;
    s_cheated = 0;
    memcpy(s_code, sys->rom.gameCode, 3);
    s_code[3] = 0;
    if (!s_client || !s_enabled) return;
    token_login();   // again when the first try had no network
    game_load();
    sys->viHook = vi_hook;
}

void XbRaGameEnd(H64System *sys)
{
    if (sys->viHook == vi_hook) sys->viHook = NULL;
    if (s_client && s_gameAsked) rc_client_unload_game(s_client);
    s_gameAsked = 0;
    s_sys = NULL;
}

void XbRaReset(void)
{
    if (!s_client || !s_enabled || !s_sys) return;
    rc_client_reset(s_client);
    // A reset game with no cheat on is clean again.
    if (s_cheated && !(s_sys->cheats && h64_cheat_active(s_sys->cheats)) && s_sys->vi.frames < 2) s_cheated = 0;
}

// ---- Save states ----
// The conditions in progress (hit counts, steps of a cup...) are kept in
// "<state file>.ra" beside each save state, as RetroAchievements asks of
// emulators: without them a loaded state started every achievement again.
static void state_side(char *out, size_t size, const char *statePath)
{
    _snprintf(out, size, "%s.ra", statePath);
    out[size - 1] = 0;
}

void XbRaStateSaved(const char *statePath)
{
    char path[200];
    size_t size;
    u8 *buf;
    FILE *f;
    state_side(path, sizeof(path), statePath);
    // A state saved without achievements must not keep an older state's progress.
    if (!s_client || !s_enabled || !s_sys || !s_gameAsked) { DeleteFileA(path); return; }
    size = rc_client_progress_size(s_client);
    buf = size ? (u8 *)malloc(size) : NULL;
    if (!buf || rc_client_serialize_progress_sized(s_client, buf, size) != RC_OK) { free(buf); DeleteFileA(path); return; }
    f = fopen(path, "wb");
    if (f)
    {
        if (fwrite(buf, 1, size, f) != size) { fclose(f); f = NULL; DeleteFileA(path); }
        else fclose(f);
    }
    free(buf);
}

void XbRaStateLoaded(const char *statePath)
{
    char path[200];
    u8 *buf = NULL;
    long size = 0;
    FILE *f;
    int r;
    if (!s_client || !s_enabled || !s_sys) return;
    state_side(path, sizeof(path), statePath);
    f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = size > 0 ? (u8 *)malloc((size_t)size) : NULL;
        if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); buf = NULL; }
        fclose(f);
    }
    // No file (an older state): rcheevos starts the conditions again.
    r = rc_client_deserialize_progress_sized(s_client, buf, buf ? (size_t)size : 0);
    H64_INFO("[ra] state loaded: achievement progress %s (%d)", buf ? "restored" : "reset", r);
    free(buf);
}

// ---- The list for the menu ----
static rc_client_achievement_list_t *s_list;
#define RA_LIST_MAX 1024
static const rc_client_achievement_t *s_listItem[RA_LIST_MAX];
static int s_listCount;

int XbRaListBegin(void)
{
    u32 b, i;
    XbRaListEnd();
    if (!s_client || !s_enabled || !s_sys || !rc_client_get_game_info(s_client)) return 0;
    s_list = rc_client_create_achievement_list(s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!s_list) return 0;
    for (b = 0; b < s_list->num_buckets; b++)
        for (i = 0; i < s_list->buckets[b].num_achievements && s_listCount < RA_LIST_MAX; i++)
            s_listItem[s_listCount++] = s_list->buckets[b].achievements[i];
    return s_listCount;
}

void XbRaListItem(int index, XbRaItem *item)
{
    const rc_client_achievement_t *a;
    memset(item, 0, sizeof(*item));
    item->title = item->description = "";
    if (index < 0 || index >= s_listCount) return;
    a = s_listItem[index];
    item->title = a->title ? a->title : "";
    item->description = a->description ? a->description : "";
    item->points = (int)a->points;
    item->unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
}

void XbRaListEnd(void)
{
    if (s_list) rc_client_destroy_achievement_list(s_list);
    s_list = NULL;
    s_listCount = 0;
}

int XbRaSelfTest(void)
{
    u8 *body = NULL;
    u32 len = 0;
    int status, id = 0;
    const char *p;
    if (!NetStart(0)) { H64_WARN("[ra] self-test: no network"); return 0; }
    status = NetPost("retroachievements.org", "/dorequest.php", "r=gameid&m=20b854b239203baf6c961b850a4a51a2", NULL, "rcheevos", &body, &len, 65536);
    p = body ? strstr((const char *)body, "\"GameID\":") : NULL;
    if (p) id = atoi(p + 9);
    H64_INFO("[ra] self-test: HTTP %d, %u bytes, game id %d: %.120s", status, len, id, body ? (const char *)body : "");
    free(body);
    return id;
}
