// VitaGitHub - browse and download GitHub release files on a PS Vita.
//
// Screens:  Repos  ->  Releases  ->  Files (+ release notes)
// Everything is fetched over HTTPS (TLS) from the public GitHub API.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <stdarg.h>
#include <ctype.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <vita2d.h>
#include "net.h"
#include "json.h"

// Bigger stack + heap: TLS handshakes and JSON need room.
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
int _newlib_heap_size_user = 128 * 1024 * 1024;

// Settings live on the memory card; downloads go to uma0 (USB / SD2Vita).
#define DATA_DIR    "ux0:data/VitaGitHub"
#define REPOS_FILE  DATA_DIR "/repos.txt"
#define TOKEN_FILE  DATA_DIR "/token.txt"
#define UMA_DL_DIR  "uma0:VitaGitHub"
#define UX_DL_DIR   DATA_DIR "/downloads"     // fallback if uma0 isn't mounted

#define SCREEN_W 960
#define SCREEN_H 544
#define LIST_Y   78
#define ROW_H    30
#define VISIBLE  14

// Colours (GitHub dark theme-ish)
#define C_BG      RGBA8(13, 17, 23, 255)
#define C_BAR     RGBA8(22, 27, 34, 255)
#define C_SEL     RGBA8(31, 111, 235, 255)
#define C_TEXT    RGBA8(230, 237, 243, 255)
#define C_DIM     RGBA8(139, 148, 158, 255)
#define C_GREEN   RGBA8(63, 185, 80, 255)
#define C_ORANGE  RGBA8(210, 153, 34, 255)
#define C_RED     RGBA8(248, 81, 73, 255)

enum { S_REPOS, S_MYREPOS, S_MENU, S_RELEASES, S_FILES, S_RUNS, S_ARTIFACTS, S_NOTES, S_COUNT };

static vita2d_pgf *font;
static int state = S_REPOS;

// Buttons (swapped automatically on Japanese consoles where O = confirm)
static uint32_t BTN_OK = SCE_CTRL_CROSS, BTN_BACK = SCE_CTRL_CIRCLE;
static const char *LBL_OK = "X", *LBL_BACK = "O";

// Input
static uint32_t pad_old, pressed;
static int hold_frames;

// Status line at the bottom
static char status_msg[256];
static uint32_t status_color;
static int status_frames;

// Saved repos
#define MAX_REPOS 64
static char repos[MAX_REPOS][128];
static int nrepos;

// Current repo / releases / files
static char cur_repo[128];
static JVal *releases_json;
#define MAX_ITEMS 100
static JVal *rel[MAX_ITEMS];
static int nrel;
static JVal *cur_rel;
static JVal *assets[MAX_ITEMS];
static int nassets;

// Release notes, pre-wrapped into lines
#define MAX_LINES 2000
static char *note_lines[MAX_LINES];
static int nlines, notes_scroll;

// Cursor for each list screen
static int sel[S_COUNT], scroll[S_COUNT];

// Signed-in account
static char token[256];
static char login[64];

// "My repositories" list (from the token's account)
static JVal *myrepos_json;
static JVal *myrepo[MAX_ITEMS];
static int nmyrepos;
static int menu_back = S_REPOS;    // where O goes from the repo menu

// Actions
static JVal *runs_json, *arts_json;
static JVal *run[MAX_ITEMS], *art[MAX_ITEMS];
static int nruns, narts;
static JVal *cur_run;

static const char *dl_root = UX_DL_DIR;   // decided at start-up

/* ================= small helpers ================= */

static void set_status(uint32_t color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
    va_end(ap);
    status_color = color;
    status_frames = 60 * 5;   // show for ~5 seconds
}

static void human_size(double bytes, char *out, size_t n) {
    if (bytes >= 1024.0 * 1024 * 1024) snprintf(out, n, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024.0 * 1024)   snprintf(out, n, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024.0)          snprintf(out, n, "%.0f KB", bytes / 1024.0);
    else                               snprintf(out, n, "%.0f B", bytes);
}

// Replace characters the memory card doesn't allow in file names.
static void safe_name(const char *in, char *out, size_t n) {
    size_t i = 0;
    for (; *in && i + 1 < n; in++)
        out[i++] = strchr(":*?\"<>|/\\", *in) ? '_' : *in;
    out[i] = 0;
}

// Draw text, shortening it with "..." if wider than maxw.
static void draw_clipped(int x, int y, uint32_t col, float scale, const char *text, int maxw) {
    if (vita2d_pgf_text_width(font, scale, text) <= maxw) {
        vita2d_pgf_draw_text(font, x, y, col, scale, text);
        return;
    }
    char buf[512];
    int len = strlen(text);
    if (len > 500) len = 500;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) len--;  // don't cut UTF-8 in half
        memcpy(buf, text, len);
        strcpy(buf + len, "...");
        if (vita2d_pgf_text_width(font, scale, buf) <= maxw) break;
    }
    vita2d_pgf_draw_text(font, x, y, col, scale, buf);
}

/* ================= drawing ================= */

static void frame_begin(const char *title, const char *subtitle) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, SCREEN_W, 56, C_BAR);
    draw_clipped(20, 36, C_TEXT, 1.2f, title, 600);
    if (subtitle) {
        int w = vita2d_pgf_text_width(font, 0.9f, subtitle);
        vita2d_pgf_draw_text(font, SCREEN_W - 20 - w, 34, C_DIM, 0.9f, subtitle);
    }
}

static void frame_end(const char *hints) {
    vita2d_draw_rectangle(0, SCREEN_H - 36, SCREEN_W, 36, C_BAR);
    if (status_frames > 0)
        draw_clipped(20, SCREEN_H - 11, status_color, 0.9f, status_msg, 920);
    else if (hints)
        vita2d_pgf_draw_text(font, 20, SCREEN_H - 11, C_DIM, 0.9f, hints);
    vita2d_end_drawing();
    vita2d_common_dialog_update();   // needed for the on-screen keyboard
    vita2d_swap_buffers();
}

// Full-screen "please wait" message (drawn twice so it really shows up).
static void show_busy(const char *msg) {
    for (int i = 0; i < 2; i++) {
        frame_begin("VitaGitHub", NULL);
        int w = vita2d_pgf_text_width(font, 1.2f, msg);
        vita2d_pgf_draw_text(font, (SCREEN_W - w) / 2, SCREEN_H / 2, C_TEXT, 1.2f, msg);
        frame_end(NULL);
    }
}

typedef void (*row_fn)(int idx, int y, int selected);

static void draw_list(int screen, int count, row_fn row) {
    int *s = &scroll[screen], cur = sel[screen];
    if (cur < *s) *s = cur;
    if (cur >= *s + VISIBLE) *s = cur - VISIBLE + 1;

    for (int i = 0; i < VISIBLE && *s + i < count; i++) {
        int idx = *s + i, y = LIST_Y + i * ROW_H;
        if (idx == cur) vita2d_draw_rectangle(10, y - 21, SCREEN_W - 30, ROW_H - 2, C_SEL);
        row(idx, y, idx == cur);
    }
    if (count > VISIBLE) {   // scrollbar
        int track = VISIBLE * ROW_H, bar = track * VISIBLE / count;
        if (bar < 20) bar = 20;
        int pos = (track - bar) * *s / (count - VISIBLE);
        vita2d_draw_rectangle(SCREEN_W - 12, LIST_Y - 21, 4, track, C_BAR);
        vita2d_draw_rectangle(SCREEN_W - 12, LIST_Y - 21 + pos, 4, bar, C_DIM);
    }
}

/* ================= input ================= */

static void read_input(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons;
    if (pad.ly < 40)  b |= SCE_CTRL_UP;      // left stick works too
    if (pad.ly > 215) b |= SCE_CTRL_DOWN;

    pressed = b & ~pad_old;
    // Holding up/down scrolls fast
    if (b & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        if (++hold_frames > 20 && hold_frames % 4 == 0) pressed |= b & (SCE_CTRL_UP | SCE_CTRL_DOWN);
    } else {
        hold_frames = 0;
    }
    pad_old = b;
}

// Move the cursor of a list screen. Returns nothing; clamps to the list.
static void move_cursor(int screen, int count) {
    int *s = &sel[screen];
    if (pressed & SCE_CTRL_UP)    (*s)--;
    if (pressed & SCE_CTRL_DOWN)  (*s)++;
    if (pressed & SCE_CTRL_LTRIGGER) *s -= VISIBLE;
    if (pressed & SCE_CTRL_RTRIGGER) *s += VISIBLE;
    if (*s >= count) *s = count - 1;
    if (*s < 0) *s = 0;
}

/* ================= on-screen keyboard ================= */

static int keyboard(const char *title, const char *initial, char *out, int maxlen) {
    static uint16_t title16[64], init16[256], text16[256];
    int i;
    for (i = 0; title[i] && i < 63; i++) title16[i] = (unsigned char)title[i];
    title16[i] = 0;
    for (i = 0; initial[i] && i < 255; i++) init16[i] = (unsigned char)initial[i];
    init16[i] = 0;
    memset(text16, 0, sizeof(text16));

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_TRUE;
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.title = title16;
    p.maxTextLength = maxlen < 255 ? maxlen : 255;
    p.initialText = init16;
    p.inputTextBuffer = text16;
    if (sceImeDialogInit(&p) < 0) return 0;

    int ok = 0;
    for (;;) {
        frame_begin("VitaGitHub", NULL);
        frame_end(NULL);
        if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult r;
            memset(&r, 0, sizeof(r));
            sceImeDialogGetResult(&r);
            if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
                for (i = 0; text16[i] && i < maxlen; i++) out[i] = text16[i] < 128 ? (char)text16[i] : '?';
                out[i] = 0;
                ok = 1;
            }
            sceImeDialogTerm();
            break;
        }
    }
    pad_old = 0xFFFFFFFF;   // ignore the button still held from the keyboard
    return ok;
}

/* ================= saved repo list ================= */

static void save_repos(void) {
    FILE *f = fopen(REPOS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < nrepos; i++) fprintf(f, "%s\n", repos[i]);
    fclose(f);
}

static void load_repos(void) {
    FILE *f = fopen(REPOS_FILE, "r");
    nrepos = 0;
    if (!f) {   // first run: a couple of examples
        strcpy(repos[nrepos++], "TheOfficialFloW/VitaShell");
        strcpy(repos[nrepos++], "Rinnegatamante/vitaQuake");
        save_repos();
        return;
    }
    char line[256];
    while (nrepos < MAX_REPOS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && strlen(line) < 128) strcpy(repos[nrepos++], line);
    }
    fclose(f);
}

// Turns "https://github.com/owner/repo.git" etc. into "owner/repo".
// Returns 0 if it doesn't look like a repo.
static int normalize_repo(const char *in, char *out) {
    char tmp[256];
    while (*in == ' ') in++;
    snprintf(tmp, sizeof(tmp), "%s", in);
    char *p = tmp;
    const char *prefixes[] = { "https://", "http://", "www.", "github.com/" };
    for (int i = 0; i < 4; i++)
        if (!strncasecmp(p, prefixes[i], strlen(prefixes[i]))) p += strlen(prefixes[i]);

    char *slash = strchr(p, '/');
    if (!slash) return 0;
    char *end = strchr(slash + 1, '/');   // drop anything after owner/repo
    if (end) *end = 0;
    int len = strlen(p);
    while (len && p[len - 1] == ' ') p[--len] = 0;
    if (len > 4 && !strcmp(p + len - 4, ".git")) p[len -= 4] = 0;

    if (slash == p || !slash[1] || len >= 128) return 0;
    for (char *c = p; *c; c++)
        if (!(isalnum((unsigned char)*c) || strchr("-_./", *c))) return 0;
    strcpy(out, p);
    return 1;
}

/* ================= GitHub API ================= */

// GET a GitHub API URL and parse it. Shows a friendly message and
// returns NULL if anything goes wrong.
static JVal *api_get(const char *url, const char *busy_msg) {
    show_busy(busy_msg);
    char *body;
    long code;
    if (net_get(url, &body, NULL, &code) < 0) {
        set_status(C_RED, "%s", net_error());
        return NULL;
    }
    JVal *root = json_parse(body);
    free(body);
    if (code == 200 && root) return root;

    const char *m = json_str(root, "message", "bad reply");
    if (code == 401)
        set_status(C_RED, "Token rejected (expired or wrong). Sign out and sign in again.");
    else if (code == 404)
        set_status(C_RED, net_has_token() ? "Not found, or your token can't see this repo."
                                          : "Not found. If it's private, sign in first.");
    else if ((code == 403 || code == 429) && strstr(m, "rate limit"))
        set_status(C_RED, net_has_token() ? "GitHub rate limit hit. Try again later."
                                          : "Rate limit hit (60/hour). Signing in raises it to 5000.");
    else if (code == 403)
        set_status(C_RED, "Permission denied: %s", m);
    else
        set_status(C_RED, "GitHub error %ld: %s", code, m);
    json_free(root);
    return NULL;
}

static void free_releases(void) { json_free(releases_json); releases_json = NULL; nrel = 0; }
static void free_runs(void)     { json_free(runs_json); runs_json = NULL; nruns = 0; }
static void free_arts(void)     { json_free(arts_json); arts_json = NULL; narts = 0; }

static int load_releases(void) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=50", cur_repo);
    JVal *root = api_get(url, "Loading releases...");
    if (!root) return 0;
    if (root->type != J_ARR) { json_free(root); set_status(C_RED, "Unexpected reply"); return 0; }

    free_releases();
    releases_json = root;
    for (JVal *r = root->child; r && nrel < MAX_ITEMS; r = r->next)
        if (!json_bool(r, "draft", 0)) rel[nrel++] = r;
    if (nrel == 0) {
        set_status(C_ORANGE, "%s has no releases.", cur_repo);
        free_releases();
        return 0;
    }
    if (sel[S_RELEASES] >= nrel) sel[S_RELEASES] = 0;
    return 1;
}

static void open_release(JVal *r) {
    cur_rel = r;
    nassets = 0;
    JVal *a = json_get(r, "assets");
    if (a && a->type == J_ARR)
        for (JVal *x = a->child; x && nassets < MAX_ITEMS; x = x->next) assets[nassets++] = x;
    sel[S_FILES] = scroll[S_FILES] = 0;
}

static int load_runs(void) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/actions/runs?per_page=40", cur_repo);
    JVal *root = api_get(url, "Loading workflow runs...");
    if (!root) return 0;
    JVal *list = json_get(root, "workflow_runs");
    if (!list || json_count(list) == 0) {
        set_status(C_ORANGE, "%s has no Actions runs.", cur_repo);
        json_free(root);
        return 0;
    }
    free_runs();
    runs_json = root;
    for (JVal *r = list->child; r && nruns < MAX_ITEMS; r = r->next) run[nruns++] = r;
    if (sel[S_RUNS] >= nruns) sel[S_RUNS] = 0;
    return 1;
}

static int load_artifacts(JVal *r) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/actions/runs/%.0f/artifacts?per_page=100",
             cur_repo, json_num(r, "id", 0));
    JVal *root = api_get(url, "Loading artifacts...");
    if (!root) return 0;
    JVal *list = json_get(root, "artifacts");
    if (!list || json_count(list) == 0) {
        const char *st = json_str(r, "status", "");
        set_status(C_ORANGE, strcmp(st, "completed") ? "Run still in progress - no artifacts yet."
                                                     : "This run has no artifacts.");
        json_free(root);
        return 0;
    }
    free_arts();
    arts_json = root;
    for (JVal *x = list->child; x && narts < MAX_ITEMS; x = x->next) art[narts++] = x;
    cur_run = r;
    sel[S_ARTIFACTS] = scroll[S_ARTIFACTS] = 0;
    return 1;
}

static int load_myrepos(void) {
    JVal *root = api_get("https://api.github.com/user/repos?per_page=100&sort=updated",
                         "Loading your repositories...");
    if (!root) return 0;
    if (root->type != J_ARR || json_count(root) == 0) {
        set_status(C_ORANGE, "No repositories visible to this token.");
        json_free(root);
        return 0;
    }
    json_free(myrepos_json);
    myrepos_json = root;
    nmyrepos = 0;
    for (JVal *r = root->child; r && nmyrepos < MAX_ITEMS; r = r->next) myrepo[nmyrepos++] = r;
    sel[S_MYREPOS] = scroll[S_MYREPOS] = 0;
    return 1;
}

/* ================= sign in / out ================= */

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    int n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int read_token_file(char *out, size_t n) {
    FILE *f = fopen(TOKEN_FILE, "r");
    if (!f) return 0;
    out[0] = 0;
    if (!fgets(out, n, f)) out[0] = 0;
    fclose(f);
    trim(out);
    return out[0] != 0;
}

// Checks the token with GitHub and remembers who we are.
static int try_token(const char *t) {
    net_set_token(t);
    JVal *me = api_get("https://api.github.com/user", "Signing in...");
    if (!me) {
        net_set_token(NULL);
        token[0] = login[0] = 0;
        return 0;
    }
    snprintf(token, sizeof(token), "%s", t);
    snprintf(login, sizeof(login), "%s", json_str(me, "login", "?"));
    json_free(me);

    FILE *f = fopen(TOKEN_FILE, "w");
    if (f) { fprintf(f, "%s\n", token); fclose(f); }
    set_status(C_GREEN, "Signed in as %s", login);
    return 1;
}

static void sign_in(void) {
    char t[256];
    // Easiest way: drop token.txt into ux0:data/VitaGitHub/ (e.g. with VitaDrop/FTP)
    if (read_token_file(t, sizeof(t)) && try_token(t)) return;
    if (keyboard("GitHub token (or put it in ux0:data/VitaGitHub/token.txt)", "", t, 255)) {
        trim(t);
        if (t[0]) try_token(t);
    }
}

static void sign_out(void) {
    net_set_token(NULL);
    token[0] = login[0] = 0;
    remove(TOKEN_FILE);
    json_free(myrepos_json);
    myrepos_json = NULL;
    nmyrepos = 0;
    set_status(C_ORANGE, "Signed out. Token deleted from the memory card.");
}

/* ================= release notes ================= */

static void free_notes(void) {
    for (int i = 0; i < nlines; i++) free(note_lines[i]);
    nlines = 0;
}

static void add_line(const char *s, int len) {
    if (nlines >= MAX_LINES) return;
    char *l = malloc(len + 1);
    if (!l) return;
    memcpy(l, s, len);
    l[len] = 0;
    for (char *c = l; *c; c++) if (*c == '\t') *c = ' ';
    note_lines[nlines++] = l;
}

// Split the notes into lines that fit on screen (word wrap).
static void wrap_notes(const char *body) {
    const float scale = 0.9f;
    const int maxw = SCREEN_W - 60;
    char tmp[1024];
    free_notes();
    notes_scroll = 0;
    if (!body || !*body) { add_line("(This release has no notes.)", 28); return; }

    const char *p = body;
    while (*p && nlines < MAX_LINES) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        int rem = len;
        if (rem && p[rem - 1] == '\r') rem--;
        const char *s = p;
        if (rem == 0) add_line("", 0);
        while (rem > 0) {
            int fit = 0, last_space = 0;
            for (int i = 1; i <= rem && i < 1000; i++) {
                if (i < rem && ((unsigned char)s[i] & 0xC0) == 0x80) continue;   // middle of a UTF-8 char
                memcpy(tmp, s, i);
                tmp[i] = 0;
                if (vita2d_pgf_text_width(font, scale, tmp) > maxw) break;
                fit = i;
                if (s[i - 1] == ' ') last_space = i;
            }
            if (fit < rem && last_space > 0) fit = last_space;
            if (fit == 0) fit = 1;
            add_line(s, fit);
            s += fit;
            rem -= fit;
            while (rem > 0 && *s == ' ') { s++; rem--; }
        }
        p = eol ? eol + 1 : p + len;
    }
}

/* ================= downloading ================= */

// Use uma0 if it's mounted, otherwise the memory card.
static void pick_download_root(void) {
    SceUID d = sceIoDopen("uma0:");
    if (d >= 0) {
        sceIoDclose(d);
        sceIoMkdir(UMA_DL_DIR, 0777);
        SceUID d2 = sceIoDopen(UMA_DL_DIR);
        if (d2 >= 0) {
            sceIoDclose(d2);
            dl_root = UMA_DL_DIR;
            return;
        }
    }
    sceIoMkdir(UX_DL_DIR, 0777);
    dl_root = UX_DL_DIR;
    set_status(C_ORANGE, "uma0: not found - saving downloads to %s", UX_DL_DIR);
}

typedef struct {
    const char *name;
    SceUInt64 start, last_draw;
} DlInfo;

static int dl_progress(void *ud, double done, double total) {
    DlInfo *d = ud;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   // keep the Vita awake

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & BTN_BACK) return 1;   // cancel

    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - d->last_draw < 100000) return 0;   // redraw max 10x per second
    d->last_draw = now;

    char a[32], b[32], speed[32], line[128];
    double secs = (now - d->start) / 1000000.0;
    human_size(done, a, sizeof(a));
    human_size(secs > 0.5 ? done / secs : 0, speed, sizeof(speed));
    if (total > 0) {
        human_size(total, b, sizeof(b));
        snprintf(line, sizeof(line), "%s / %s   (%s/s)", a, b, speed);
    } else {
        snprintf(line, sizeof(line), "%s   (%s/s)", a, speed);
    }

    frame_begin("Downloading", cur_repo);
    draw_clipped(60, 200, C_TEXT, 1.1f, d->name, SCREEN_W - 120);
    vita2d_draw_rectangle(60, 230, SCREEN_W - 120, 28, C_BAR);
    if (total > 0)
        vita2d_draw_rectangle(60, 230, (SCREEN_W - 120) * (done / total), 28, C_GREEN);
    vita2d_pgf_draw_text(font, 60, 290, C_DIM, 1.0f, line);
    char hint[64];
    snprintf(hint, sizeof(hint), "%s: Cancel", LBL_BACK);
    frame_end(hint);
    return 0;
}

// Saves to <uma0:VitaGitHub>/<owner_repo>/<filename>
static void download(const char *url, const char *accept, const char *filename) {
    char dir[256], path[512], fname[256], repo_dir[128];
    if (!url || !url[0]) { set_status(C_RED, "No download link for this file."); return; }
    safe_name(cur_repo, repo_dir, sizeof(repo_dir));
    safe_name(filename, fname, sizeof(fname));
    snprintf(dir, sizeof(dir), "%s/%s", dl_root, repo_dir);
    sceIoMkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%s", dir, fname);

    show_busy("Connecting...");
    DlInfo d = { fname, sceKernelGetProcessTimeWide(), 0 };
    int r = net_download(url, accept, path, dl_progress, &d);
    pad_old = 0xFFFFFFFF;

    if (r == 0) {
        int is_vpk = strlen(fname) > 4 && !strcasecmp(fname + strlen(fname) - 4, ".vpk");
        set_status(C_GREEN, "Saved to %s%s", path, is_vpk ? "  - install it with VitaShell" : "");
    } else if (r == 1) {
        set_status(C_ORANGE, "Download cancelled.");
    } else {
        set_status(C_RED, "Download failed: %s", net_error());
    }
}

/* ================= list rows ================= */

static void row_repo(int i, int y, int s) {
    (void)s;
    if (i == 0) {
        vita2d_pgf_draw_text(font, 24, y, C_GREEN, 1.0f, "+ Add repository...");
    } else if (i == 1) {
        char t[128];
        if (login[0]) snprintf(t, sizeof(t), "My repositories  (signed in as %s)", login);
        else if (token[0]) snprintf(t, sizeof(t), "My repositories");
        else snprintf(t, sizeof(t), "Sign in with a GitHub token...");
        vita2d_pgf_draw_text(font, 24, y, C_GREEN, 1.0f, t);
    } else {
        draw_clipped(24, y, C_TEXT, 1.0f, repos[i - 2], SCREEN_W - 70);
    }
}

static void row_myrepo(int i, int y, int s) {
    JVal *r = myrepo[i];
    draw_clipped(24, y, C_TEXT, 1.0f, json_str(r, "full_name", "?"), 640);
    if (json_bool(r, "private", 0))
        vita2d_pgf_draw_text(font, 700, y, s ? C_TEXT : C_ORANGE, 0.9f, "Private");
    char date[16];
    snprintf(date, sizeof(date), "%.10s", json_str(r, "pushed_at", ""));
    vita2d_pgf_draw_text(font, 800, y, s ? C_TEXT : C_DIM, 0.9f, date);
}

static const char *menu_items[] = { "Releases", "Actions  (workflow runs & build artifacts)", "Save to my repo list" };

static int is_saved(const char *repo) {
    for (int k = 0; k < nrepos; k++) if (!strcasecmp(repos[k], repo)) return 1;
    return 0;
}

static int menu_count(void) { return is_saved(cur_repo) ? 2 : 3; }

static void row_menu(int i, int y, int s) {
    (void)s;
    vita2d_pgf_draw_text(font, 24, y, C_TEXT, 1.0f, menu_items[i]);
}

static void row_release(int i, int y, int s) {
    JVal *r = rel[i];
    const char *tag = json_str(r, "tag_name", "?");
    const char *name = json_str(r, "name", "");
    char date[16];
    snprintf(date, sizeof(date), "%.10s", json_str(r, "published_at", ""));

    char text[300];
    if (name[0] && strcmp(name, tag)) snprintf(text, sizeof(text), "%s  -  %s", tag, name);
    else snprintf(text, sizeof(text), "%s", tag);
    draw_clipped(24, y, C_TEXT, 1.0f, text, 600);

    if (i == 0 && !json_bool(r, "prerelease", 0))
        vita2d_pgf_draw_text(font, 650, y, s ? C_TEXT : C_GREEN, 0.9f, "Latest");
    else if (json_bool(r, "prerelease", 0))
        vita2d_pgf_draw_text(font, 650, y, s ? C_TEXT : C_ORANGE, 0.9f, "Pre-release");
    vita2d_pgf_draw_text(font, 800, y, s ? C_TEXT : C_DIM, 0.9f, date);
}

static void row_file(int i, int y, int s) {
    if (i == nassets) {   // last row: GitHub's auto-generated source zip
        vita2d_pgf_draw_text(font, 24, y, s ? C_TEXT : C_DIM, 1.0f, "Source code (zip)");
        return;
    }
    JVal *a = assets[i];
    char size[32], dls[32];
    human_size(json_num(a, "size", 0), size, sizeof(size));
    snprintf(dls, sizeof(dls), "%.0f dl", json_num(a, "download_count", 0));
    draw_clipped(24, y, C_TEXT, 1.0f, json_str(a, "name", "?"), 640);
    vita2d_pgf_draw_text(font, 690, y, s ? C_TEXT : C_DIM, 0.9f, size);
    vita2d_pgf_draw_text(font, 820, y, s ? C_TEXT : C_DIM, 0.9f, dls);
}

static void row_run(int i, int y, int s) {
    JVal *r = run[i];
    const char *title = json_str(r, "display_title", json_str(r, "name", "?"));
    char text[300];
    snprintf(text, sizeof(text), "#%.0f  %s", json_num(r, "run_number", 0), title);
    draw_clipped(24, y, C_TEXT, 1.0f, text, 520);
    draw_clipped(560, y, s ? C_TEXT : C_DIM, 0.9f, json_str(r, "head_branch", ""), 120);

    const char *st = json_str(r, "status", "");
    const char *cc = json_str(r, "conclusion", "");
    const char *label;
    uint32_t col;
    if (strcmp(st, "completed"))          { label = !strcmp(st, "queued") ? "queued" : "running"; col = C_ORANGE; }
    else if (!strcmp(cc, "success"))      { label = "passed"; col = C_GREEN; }
    else if (!strcmp(cc, "failure"))      { label = "failed"; col = C_RED; }
    else                                  { label = cc[0] ? cc : "done"; col = C_DIM; }
    vita2d_pgf_draw_text(font, 700, y, s ? C_TEXT : col, 0.9f, label);

    char date[16];
    snprintf(date, sizeof(date), "%.10s", json_str(r, "created_at", ""));
    vita2d_pgf_draw_text(font, 820, y, s ? C_TEXT : C_DIM, 0.9f, date);
}

static void row_artifact(int i, int y, int s) {
    JVal *a = art[i];
    char size[32];
    human_size(json_num(a, "size_in_bytes", 0), size, sizeof(size));
    draw_clipped(24, y, C_TEXT, 1.0f, json_str(a, "name", "?"), 640);
    vita2d_pgf_draw_text(font, 690, y, s ? C_TEXT : C_DIM, 0.9f, size);
    if (json_bool(a, "expired", 0))
        vita2d_pgf_draw_text(font, 820, y, s ? C_TEXT : C_RED, 0.9f, "expired");
    else
        vita2d_pgf_draw_text(font, 820, y, s ? C_TEXT : C_DIM, 0.9f, ".zip");
}

/* ================= screens ================= */

static void go_menu(const char *repo, int back) {
    snprintf(cur_repo, sizeof(cur_repo), "%s", repo);
    menu_back = back;
    sel[S_MENU] = scroll[S_MENU] = 0;
    sel[S_RELEASES] = scroll[S_RELEASES] = 0;
    sel[S_RUNS] = scroll[S_RUNS] = 0;
    state = S_MENU;
}

static void screen_repos(void) {
    int count = nrepos + 2;
    move_cursor(S_REPOS, count);
    int i = sel[S_REPOS];

    if (pressed & BTN_OK) {
        if (i == 0) {
            char input[200], repo[128];
            if (keyboard("Enter owner/repo or a GitHub link", "", input, 199)) {
                if (!normalize_repo(input, repo)) {
                    set_status(C_RED, "That doesn't look like owner/repo");
                } else {
                    if (!is_saved(repo) && nrepos < MAX_REPOS) {
                        strcpy(repos[nrepos++], repo);
                        save_repos();
                    }
                    go_menu(repo, S_REPOS);
                }
            }
        } else if (i == 1) {
            if (!token[0]) sign_in();
            else if (load_myrepos()) state = S_MYREPOS;
        } else {
            go_menu(repos[i - 2], S_REPOS);
        }
    } else if ((pressed & SCE_CTRL_TRIANGLE) && i > 1) {
        set_status(C_ORANGE, "Removed %s", repos[i - 2]);
        memmove(repos[i - 2], repos[i - 1], (nrepos - (i - 1)) * sizeof(repos[0]));
        nrepos--;
        save_repos();
    } else if ((pressed & SCE_CTRL_SELECT) && token[0]) {
        sign_out();
    }

    char hint[160];
    snprintf(hint, sizeof(hint), "%s: Open    Triangle: Remove    %sSTART: Exit",
             LBL_OK, token[0] ? "SELECT: Sign out    " : "");
    frame_begin("VitaGitHub", "Repositories");
    draw_list(S_REPOS, count, row_repo);
    frame_end(hint);
}

static void screen_myrepos(void) {
    move_cursor(S_MYREPOS, nmyrepos);
    if (pressed & BTN_OK) {
        go_menu(json_str(myrepo[sel[S_MYREPOS]], "full_name", ""), S_MYREPOS);
        return;
    }
    if (pressed & BTN_BACK) { state = S_REPOS; return; }

    char hint[128], sub[96];
    snprintf(hint, sizeof(hint), "%s: Open    %s: Back", LBL_OK, LBL_BACK);
    snprintf(sub, sizeof(sub), "%s's repositories", login[0] ? login : "Your");
    frame_begin("My repositories", sub);
    draw_list(S_MYREPOS, nmyrepos, row_myrepo);
    frame_end(hint);
}

static void screen_menu(void) {
    int count = menu_count();
    move_cursor(S_MENU, count);
    if (pressed & BTN_OK) {
        int i = sel[S_MENU];
        if (i == 0 && load_releases()) state = S_RELEASES;
        else if (i == 1 && load_runs()) state = S_RUNS;
        else if (i == 2 && nrepos < MAX_REPOS) {
            strcpy(repos[nrepos++], cur_repo);
            save_repos();
            set_status(C_GREEN, "Added %s to your list", cur_repo);
            sel[S_MENU] = 0;
        }
    } else if (pressed & BTN_BACK) {
        state = menu_back;
        return;
    }
    char hint[128];
    snprintf(hint, sizeof(hint), "%s: Open    %s: Back", LBL_OK, LBL_BACK);
    frame_begin(cur_repo, NULL);
    draw_list(S_MENU, count, row_menu);
    frame_end(hint);
}

static void screen_releases(void) {
    move_cursor(S_RELEASES, nrel);
    if (pressed & BTN_OK) {
        open_release(rel[sel[S_RELEASES]]);
        state = S_FILES;
    } else if (pressed & BTN_BACK) {
        free_releases();
        state = S_MENU;
        return;
    } else if (pressed & SCE_CTRL_SELECT) {
        if (!load_releases()) { state = S_MENU; return; }
    }

    char hint[128];
    snprintf(hint, sizeof(hint), "%s: Open    %s: Back    SELECT: Refresh", LBL_OK, LBL_BACK);
    frame_begin(cur_repo, "Releases");
    draw_list(S_RELEASES, nrel, row_release);
    frame_end(hint);
}

static void screen_files(void) {
    int count = nassets + 1;
    move_cursor(S_FILES, count);
    int i = sel[S_FILES];
    const char *tag = json_str(cur_rel, "tag_name", "release");

    if (pressed & BTN_OK) {
        if (i == nassets) {
            const char *slash = strchr(cur_repo, '/');
            char name[256];
            snprintf(name, sizeof(name), "%s-%s.zip", slash ? slash + 1 : cur_repo, tag);
            download(json_str(cur_rel, "zipball_url", ""), NULL, name);
        } else if (token[0]) {
            // Signed in: use the API link, which also works for private repos
            download(json_str(assets[i], "url", ""), "application/octet-stream",
                     json_str(assets[i], "name", "file"));
        } else {
            download(json_str(assets[i], "browser_download_url", ""), NULL,
                     json_str(assets[i], "name", "file"));
        }
    } else if (pressed & SCE_CTRL_TRIANGLE) {
        wrap_notes(json_str(cur_rel, "body", ""));
        state = S_NOTES;
    } else if (pressed & BTN_BACK) {
        state = S_RELEASES;
        return;
    }

    char title[256], hint[128];
    snprintf(title, sizeof(title), "%s  %s", cur_repo, tag);
    snprintf(hint, sizeof(hint), "%s: Download    Triangle: Release notes    %s: Back", LBL_OK, LBL_BACK);
    frame_begin(title, "Files");
    draw_list(S_FILES, count, row_file);
    frame_end(hint);
}

static void screen_runs(void) {
    move_cursor(S_RUNS, nruns);
    if (pressed & BTN_OK) {
        if (load_artifacts(run[sel[S_RUNS]])) state = S_ARTIFACTS;
    } else if (pressed & BTN_BACK) {
        free_runs();
        state = S_MENU;
        return;
    } else if (pressed & SCE_CTRL_SELECT) {
        if (!load_runs()) { state = S_MENU; return; }
    }

    char hint[128];
    snprintf(hint, sizeof(hint), "%s: Artifacts    %s: Back    SELECT: Refresh", LBL_OK, LBL_BACK);
    frame_begin(cur_repo, "Actions");
    if (nruns) draw_list(S_RUNS, nruns, row_run);
    frame_end(hint);
}

static void screen_artifacts(void) {
    move_cursor(S_ARTIFACTS, narts);
    if (pressed & BTN_OK) {
        JVal *a = art[sel[S_ARTIFACTS]];
        char name[200];
        snprintf(name, sizeof(name), "%s-run%.0f.zip", json_str(a, "name", "artifact"),
                 json_num(cur_run, "run_number", 0));
        if (!token[0])
            set_status(C_RED, "GitHub only allows artifact downloads when signed in.");
        else if (json_bool(a, "expired", 0))
            set_status(C_RED, "This artifact has expired.");
        else
            download(json_str(a, "archive_download_url", ""), NULL, name);
    } else if (pressed & BTN_BACK) {
        free_arts();
        state = S_RUNS;
        return;
    }

    char title[256], hint[128];
    snprintf(title, sizeof(title), "#%.0f %s", json_num(cur_run, "run_number", 0),
             json_str(cur_run, "display_title", ""));
    snprintf(hint, sizeof(hint), "%s: Download    %s: Back", LBL_OK, LBL_BACK);
    frame_begin(title, "Artifacts");
    draw_list(S_ARTIFACTS, narts, row_artifact);
    frame_end(hint);
}

static void screen_notes(void) {
    const int per_page = 16;
    if (pressed & SCE_CTRL_UP)   notes_scroll--;
    if (pressed & SCE_CTRL_DOWN) notes_scroll++;
    if (pressed & SCE_CTRL_LTRIGGER) notes_scroll -= per_page;
    if (pressed & SCE_CTRL_RTRIGGER) notes_scroll += per_page;
    if (notes_scroll > nlines - per_page) notes_scroll = nlines - per_page;
    if (notes_scroll < 0) notes_scroll = 0;
    if (pressed & (BTN_BACK | SCE_CTRL_TRIANGLE)) {
        free_notes();
        state = S_FILES;
        return;
    }

    char title[256], hint[64];
    snprintf(title, sizeof(title), "%s  %s", cur_repo, json_str(cur_rel, "tag_name", ""));
    snprintf(hint, sizeof(hint), "Up/Down/L/R: Scroll    %s: Back", LBL_BACK);
    frame_begin(title, "Release notes");
    for (int k = 0; k < per_page && notes_scroll + k < nlines; k++)
        vita2d_pgf_draw_text(font, 30, 84 + k * 26, C_TEXT, 0.9f, note_lines[notes_scroll + k]);
    frame_end(hint);
}

/* ================= main ================= */

int main(void) {
    // System dialogs (keyboard) need these
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);

    SceCommonDialogConfigParam cfg;
    sceCommonDialogConfigParamInit(&cfg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cfg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cfg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&cfg);
    if (cfg.enterButtonAssign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
        BTN_OK = SCE_CTRL_CIRCLE; BTN_BACK = SCE_CTRL_CROSS;
        LBL_OK = "O"; LBL_BACK = "X";
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita2d_init();
    vita2d_set_clear_color(C_BG);
    font = vita2d_load_default_pgf();

    sceIoMkdir(DATA_DIR, 0777);
    load_repos();
    pick_download_root();

    if (net_init() < 0) {
        set_status(C_RED, "%s", net_error());
    } else {
        char t[256];
        if (read_token_file(t, sizeof(t))) try_token(t);   // auto sign-in
    }
    pad_old = 0xFFFFFFFF;   // ignore the button used to launch the app

    for (;;) {
        read_input();
        if (pressed & SCE_CTRL_START) break;
        if (status_frames > 0) status_frames--;

        switch (state) {
        case S_REPOS:     screen_repos(); break;
        case S_MYREPOS:   screen_myrepos(); break;
        case S_MENU:      screen_menu(); break;
        case S_RELEASES:  screen_releases(); break;
        case S_FILES:     screen_files(); break;
        case S_RUNS:      screen_runs(); break;
        case S_ARTIFACTS: screen_artifacts(); break;
        case S_NOTES:     screen_notes(); break;
        }
    }

    free_notes();
    free_releases();
    free_runs();
    free_arts();
    json_free(myrepos_json);
    net_exit();
    vita2d_fini();
    vita2d_free_pgf(font);
    sceKernelExitProcess(0);
    return 0;
}
