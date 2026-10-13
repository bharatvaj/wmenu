#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <limits.h>
#include <stdint.h>
#include <wctype.h>

#include "arg.h"
#ifdef _MSC_VER
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define ID_COMBO 101
#define ID_EDIT 102
#define ID_LABEL 103
#define ID_PROGRESS 104
#define TIMER_PROGRESS 201
#define TIMER_SEARCH 202
#define WM_STDIN_CHUNK (WM_APP + 1)
#define WM_STDIN_DONE (WM_APP + 2)
#define WM_SEARCH_STEP (WM_APP + 3)
#define SEARCH_BATCH 64
#define CHUNK_LINES 2048
#define LOCATION_LIMIT 128

static int dpi = 96;
#define S(px) MulDiv((px), dpi, 96)

static char **items, **selected;
static volatile LONG item_count; /* Published after items[] is populated. */
static int item_capacity, selected_count;
static int *matches, match_count, match_capacity, search_pos;
static char *search_query;
struct location { char *query; int item; }; /* Source item IDs survive filtering. */
static struct location locations[LOCATION_LIMIT];
static int location_count, location_cursor, last_row = -1, pending_item = -1;
static bool restoring_location, search_posted;
static volatile LONG refresh_pending;
static bool has_items, has_prompt, multi_select, vi_mode, normal_mode, default_controls;
static wchar_t prompt[300];
static HWND main_hwnd, input_hwnd, list_hwnd, progress_hwnd;
static WNDPROC old_input_proc, old_list_proc;
static LPVOID ui_fiber, search_fiber;
static CRITICAL_SECTION items_lock;
static int win_x = 0, win_y = 0, win_w = 0, win_h = 0, lines = 5;
static int font_size = 0, font_weight = FW_NORMAL;
static UINT text_codepage = CP_UTF8;
static wchar_t font_name[LF_FACESIZE];
static HFONT ui_font;
static bool custom_font;
static bool center_window, close_on_blur, stdin_pipe, loading_items, input_done, progress_visible;
static int progress_pos, progress_dir = 5;
char *argv0;

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(text_codepage, 0, s ? s : "", -1, NULL, 0);
    wchar_t *w = (wchar_t *)calloc(n ? n : 1, sizeof(wchar_t));
    if (w) MultiByteToWideChar(text_codepage, 0, s ? s : "", -1, w, n);
    return w;
}

static char *text_encoded(HWND hwnd)
{
    int n = GetWindowTextLengthW(hwnd) + 1;
    wchar_t *w = (wchar_t *)calloc(n, sizeof(wchar_t));
    char *s = NULL;
    int m;
    if (!w) return NULL;
    GetWindowTextW(hwnd, w, n);
    m = WideCharToMultiByte(text_codepage, 0, w, -1, NULL, 0, NULL, NULL);
    s = (char *)calloc(m ? m : 1, 1);
    if (s) WideCharToMultiByte(text_codepage, 0, w, -1, s, m, NULL, NULL);
    free(w);
    return s;
}

static int loaded_count(void)
{
    return (int)InterlockedCompareExchange(&item_count, 0, 0);
}

static bool add_item(const char *s)
{
    char *copy = _strdup(s ? s : "");
    if (!copy) return false;
    EnterCriticalSection(&items_lock);
    if (item_count == item_capacity) {
        if (item_capacity > INT_MAX / 2) {
            LeaveCriticalSection(&items_lock);
            free(copy);
            return false;
        }
        int next = item_capacity ? item_capacity * 2 : 1024;
        if ((size_t)next > SIZE_MAX / sizeof(*items)) {
            LeaveCriticalSection(&items_lock);
            free(copy);
            return false;
        }
        char **grown = (char **)realloc(items, (size_t)next * sizeof(*items));
        if (!grown) {
            LeaveCriticalSection(&items_lock);
            free(copy);
            return false;
        }
        items = grown;
        item_capacity = next;
    }
    items[item_count] = copy;
    InterlockedIncrement(&item_count);
    LeaveCriticalSection(&items_lock);
    return true;
}

static void make_font(void)
{
    ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (custom_font) {
        LOGFONTW lf;
        HDC dc = GetDC(NULL);
        GetObjectW(ui_font, sizeof lf, &lf);
        if (font_name[0]) { wcsncpy(lf.lfFaceName, font_name, LF_FACESIZE - 1); lf.lfFaceName[LF_FACESIZE - 1] = 0; }
        if (font_size > 0) lf.lfHeight = -MulDiv(font_size, GetDeviceCaps(dc, LOGPIXELSY), 72);
        lf.lfWeight = font_weight;
        ReleaseDC(NULL, dc);
        ui_font = CreateFontIndirectW(&lf);
    }
}

static void set_font(HWND hwnd)
{
    if (ui_font && hwnd) SendMessageW(hwnd, WM_SETFONT, (WPARAM)ui_font, TRUE);
}

/* Read one complete line, growing the buffer rather than splitting long rows. */
static int read_line(FILE *stream, char **line, size_t *capacity)
{
    size_t len = 0;
    for (;;) {
        if (*capacity - len < 2) {
            if (*capacity > INT_MAX / 2) return -1;
            size_t next = *capacity ? *capacity * 2 : 4096;
            char *grown = (char *)realloc(*line, next);
            if (!grown) return -1;
            *line = grown;
            *capacity = next;
        }
        if (!fgets(*line + len, (int)(*capacity - len), stream)) {
            if (ferror(stream)) return -1;
            if (!len) return 0;
            break;
        }
        len += strlen(*line + len);
        if (len && (*line)[len - 1] == '\n') break;
    }
    if (len && (*line)[len - 1] == '\n') --len;
    if (len && (*line)[len - 1] == '\r') --len;
    (*line)[len] = 0;
    return 1;
}

static DWORD WINAPI read_stdin_thread(void *unused)
{
    char *line = NULL;
    size_t capacity = 0;
    int n = 0, status;
    (void)unused;
    while ((status = read_line(stdin, &line, &capacity)) > 0) {
        if (*line) {
            if (!add_item(line)) { status = -1; break; }
            if ((++n == 1 || n % CHUNK_LINES == 0) && !InterlockedExchange(&refresh_pending, 1))
                PostMessageW(main_hwnd, WM_STDIN_CHUNK, 0, 0);
        }
    }
    free(line);
    PostMessageW(main_hwnd, WM_STDIN_DONE, status < 0, 0);
    return 0;
}

/* fzy-style subsequence matching, with non-ASCII UTF-8 bytes compared literally. */
static bool has_match(const char *query, const char *line)
{
    while (*query) {
        unsigned char ch = (unsigned char)*query++;
        unsigned char upper = ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch;
        while (*line && (unsigned char)*line != ch && (unsigned char)*line != upper) ++line;
        if (!*line) return false;
        ++line;
    }
    return true;
}

/* The ListBox stores only a count. Its row IDs map into items or matches. */
static int row_item(int row)
{
    if (row < 0) return -1;
    if (search_query && *search_query)
        return row < match_count ? matches[row] : -1;
    return row < loaded_count() ? row : -1;
}

static void truncate_locations(void)
{
    for (int i = location_cursor + 1; i < location_count; ++i) free(locations[i].query);
    if (location_count) location_count = location_cursor + 1;
}

static void push_location(const char *query, int item)
{
    char *copy = _strdup(query ? query : "");
    if (!copy) return;
    truncate_locations();
    if (location_count == LOCATION_LIMIT) {
        free(locations[0].query);
        memmove(locations, locations + 1, (LOCATION_LIMIT - 1) * sizeof(*locations));
        --location_count;
    }
    locations[location_count++] = (struct location){ copy, item };
    location_cursor = location_count - 1;
}

static void record_location(int from, int to)
{
    if (to < 0 || from == to) return;
    int new_item = row_item(to);
    if (from < 0) {
        if (!location_count || pending_item >= 0 ||
            strcmp(locations[location_cursor].query, search_query ? search_query : ""))
            push_location(search_query, new_item);
        else locations[location_cursor].item = new_item;
        return;
    }
    int old_item = row_item(from);
    if (!location_count) push_location(search_query, old_item);
    else if (!strcmp(locations[location_cursor].query, search_query ? search_query : "") &&
             locations[location_cursor].item < 0)
        locations[location_cursor].item = old_item;
    else if (strcmp(locations[location_cursor].query, search_query ? search_query : "") ||
             locations[location_cursor].item != old_item)
        push_location(search_query, old_item);
    push_location(search_query, new_item);
}

static void restore_location(void)
{
    if (pending_item < 0) return;
    int row = pending_item;
    if (search_query && *search_query) {
        if (search_pos <= pending_item) return; /* Search has not reached this item. */
        int lo = 0, hi = match_count;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (matches[mid] < pending_item) lo = mid + 1;
            else hi = mid;
        }
        row = lo < match_count && matches[lo] == pending_item ? lo : -1;
    }
    pending_item = -1;
    if (row >= 0 && row < SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0)) {
        int previous = (int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0);
        SendMessageW(list_hwnd, LB_SETCURSEL, row, 0);
        last_row = row;
        RECT rect;
        if (previous >= 0 && SendMessageW(list_hwnd, LB_GETITEMRECT, previous, (LPARAM)&rect) != LB_ERR)
            InvalidateRect(list_hwnd, &rect, FALSE);
        if (SendMessageW(list_hwnd, LB_GETITEMRECT, row, (LPARAM)&rect) != LB_ERR)
            InvalidateRect(list_hwnd, &rect, FALSE);
        UpdateWindow(list_hwnd);
    }
}

static void visit_location(int delta)
{
    int next = location_cursor + delta;
    if (!list_hwnd || !location_count || next < 0 || next >= location_count) return;
    wchar_t *query = widen(locations[next].query);
    if (!query) return;
    location_cursor = next;
    pending_item = locations[next].item;
    restoring_location = true;
    SetWindowTextW(input_hwnd, query); /* EN_CHANGE starts the saved search. */
    restoring_location = false;
    free(query);
    if (pending_item >= 0 && search_query && *search_query && search_pos <= pending_item) {
        SendMessageW(list_hwnd, LB_SETCURSEL, -1, 0);
        last_row = -1;
        InvalidateRect(list_hwnd, NULL, FALSE);
        UpdateWindow(list_hwnd);
    }
    restore_location();
}

static void update_count(void)
{
    int count = search_query && *search_query ? match_count : loaded_count();
    int old_count = (int)SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0);
    if (old_count == count) return;
    int selected = (int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0);
    int top = (int)SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0);
    int height = (int)SendMessageW(list_hwnd, LB_GETITEMHEIGHT, 0, 0);
    RECT client, pending;
    bool had_pending = GetUpdateRect(list_hwnd, &pending, FALSE) && !IsRectEmpty(&pending);
    GetClientRect(list_hwnd, &client);
    SendMessageW(list_hwnd, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_hwnd, LB_SETCOUNT, count, 0);
    if (count) {
        int wanted = pending_item >= 0 ? -1 : selected >= 0 && selected < count ? selected : 0;
        SendMessageW(list_hwnd, LB_SETCURSEL, wanted, 0);
        last_row = wanted;
        if (top > 0 || SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0) != 0)
            SendMessageW(list_hwnd, LB_SETTOPINDEX, top < count ? top : count - 1, 0);
    }
    SendMessageW(list_hwnd, WM_SETREDRAW, TRUE, 0);
    /* LB_SETCOUNT may invalidate everything. Only newly visible rows need paint. */
    RedrawWindow(list_hwnd, NULL, NULL, RDW_VALIDATE | RDW_NOCHILDREN);
    int new_top = (int)SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0);
    if (top != new_top || count < old_count || height <= 0) {
        InvalidateRect(list_hwnd, NULL, FALSE);
    } else if (old_count < new_top + (client.bottom - client.top + height - 1) / height) {
        client.top += (old_count - new_top) * height;
        if (client.top < 0) client.top = 0;
        InvalidateRect(list_hwnd, &client, FALSE);
    }
    if (had_pending) InvalidateRect(list_hwnd, &pending, FALSE);
    UpdateWindow(list_hwnd); /* Do not starve new rows or selection paints. */
}

static void update_progress(void)
{
    int count = loaded_count();
    bool searching = input_done && search_query && *search_query && search_pos < count;
    bool visible = !input_done || searching;
    if (visible != progress_visible) {
        ShowWindow(progress_hwnd, visible ? SW_SHOW : SW_HIDE);
        SetWindowPos(list_hwnd, NULL, 0, S(28), win_w,
                     win_h - S(28) - (visible ? S(10) : 0), SWP_NOZORDER);
        progress_visible = visible;
    }
    if (searching)
        SendMessageW(progress_hwnd, PBM_SETPOS,
                     count ? (int)(100LL * search_pos / count) : 100, 0);
}

static void queue_search(void)
{
    if (!search_posted) search_posted = PostMessageW(main_hwnd, WM_SEARCH_STEP, 0, 0) != 0;
}

static VOID WINAPI search_fiber_run(void *unused)
{
    (void)unused;
    for (;;) {
        EnterCriticalSection(&items_lock);
        int remaining = item_count - search_pos;
        int end = search_pos + (remaining < SEARCH_BATCH ? remaining : SEARCH_BATCH);
        for (; search_pos < end; ++search_pos) {
            if (!has_match(search_query, items[search_pos])) continue;
            if (match_count == match_capacity) {
                if (match_capacity > INT_MAX / 2) { PostQuitMessage(1); search_pos = item_count; break; }
                int next = match_capacity ? match_capacity * 2 : 1024;
                if ((size_t)next > SIZE_MAX / sizeof(*matches)) { PostQuitMessage(1); search_pos = item_count; break; }
                int *grown = (int *)realloc(matches, (size_t)next * sizeof(*matches));
                if (!grown) { PostQuitMessage(1); search_pos = item_count; break; }
                matches = grown;
                match_capacity = next;
            }
            matches[match_count++] = search_pos;
        }
        LeaveCriticalSection(&items_lock);
        SwitchToFiber(ui_fiber);  /* Never yield while holding items_lock. */
    }
}

static void search_step(void)
{
    if (!search_query || !*search_query || search_pos >= loaded_count()) {
        KillTimer(main_hwnd, TIMER_SEARCH);
        return;
    }
    SwitchToFiber(search_fiber);
    update_count();
    restore_location();
    update_progress();
    if (search_pos >= loaded_count()) KillTimer(main_hwnd, TIMER_SEARCH);
    else if (HIWORD(GetQueueStatus(QS_INPUT)))
        SetTimer(main_hwnd, TIMER_SEARCH, 15, NULL); /* Let keyboard/mouse go first. */
    else queue_search();
}

static void refill_list(const char *query)
{
    if (!query) return;
    if (!search_query || strcmp(query, search_query)) {
        char *copy = _strdup(query);
        if (!copy) return;
        if (!restoring_location) {
            pending_item = -1;
            if (search_query) {
                int item = row_item((int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0));
                if (!location_count) push_location(search_query, item);
                else if (!strcmp(locations[location_cursor].query, search_query) && item >= 0)
                    locations[location_cursor].item = item;
                bool related = *query && *search_query && last_row <= 0 &&
                    (!strncmp(query, search_query, strlen(query)) ||
                     !strncmp(query, search_query, strlen(search_query)));
                bool intermediate_empty = !*search_query && *query && last_row <= 0 &&
                    location_count > 1 && !*locations[location_cursor].query;
                if (related || intermediate_empty) {
                    char *history_copy = _strdup(query);
                    if (!history_copy) { free(copy); return; }
                    truncate_locations();
                    free(locations[location_cursor].query);
                    locations[location_cursor] = (struct location){ history_copy, -1 };
                } else push_location(query, -1);
            }
        }
        free(search_query);
        search_query = copy;
        match_count = 0;
        search_pos = 0;
        KillTimer(main_hwnd, TIMER_SEARCH);
        last_row = -1;
        if (*search_query) SendMessageW(list_hwnd, LB_SETCOUNT, 0, 0);
        else update_count();
        SendMessageW(list_hwnd, LB_SETTOPINDEX, 0, 0);
        if (SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) > 0) {
            SendMessageW(list_hwnd, LB_SETCURSEL, 0, 0);
            last_row = 0;
        }
        UpdateWindow(list_hwnd);
        restore_location();
    }
    if (*search_query && search_pos < loaded_count()) queue_search();
    else if (!*search_query) update_count();
    if (progress_hwnd) update_progress();
}

static char *current_text(void)
{
    int sel = list_hwnd ? (int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) : -1;
    if (has_items && (sel >= 0 || pending_item >= 0)) {
        char *s = NULL;
        EnterCriticalSection(&items_lock);
        int index = pending_item >= 0 ? pending_item : row_item(sel);
        if (index >= 0 && index < item_count) s = _strdup(items[index]);
        LeaveCriticalSection(&items_lock);
        return s;
    }
    return text_encoded(input_hwnd);
}

static void add_selection(void)
{
    char *s = current_text();
    if (!s) { fputs("wmenu: out of memory selecting item\n", stderr); PostQuitMessage(1); return; }
    if (!*s) { free(s); return; }
    if (selected_count == INT_MAX || (size_t)(selected_count + 1) > SIZE_MAX / sizeof(*selected)) {
        free(s);
        fputs("wmenu: too many selections\n", stderr);
        PostQuitMessage(1);
        return;
    }
    char **grown = (char **)realloc(selected, sizeof(*selected) * (size_t)(selected_count + 1));
    if (!grown) {
        free(s);
        fputs("wmenu: out of memory selecting item\n", stderr);
        PostQuitMessage(1);
        return;
    }
    selected = grown;
    selected[selected_count++] = s;
    SetWindowTextW(input_hwnd, L"");
    if (has_items) refill_list("");
}

static void finish(void)
{
    char *s = input_hwnd ? current_text() : NULL;
    if (input_hwnd && !s) { fputs("wmenu: out of memory accepting item\n", stderr); PostQuitMessage(1); return; }
    for (int i = 0; i < selected_count; ++i) printf("%s\n", selected[i]);
    if (s && *s) fputs(s, stdout);
    free(s);
    PostQuitMessage(0);
}

static void list_move(int delta)
{
    if (!has_items) return;
    LRESULT count = SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0);
    LRESULT cur = SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0);
    LRESULT previous = cur;
    if (count <= 0) return;
    if (cur < 0) cur = delta > 0 ? -1 : count;
    cur += delta;
    if (cur < 0) cur = 0;
    if (cur >= count) cur = count - 1;
    if (cur == previous) return;
    SendMessageW(list_hwnd, LB_SETCURSEL, cur, 0);
    record_location((int)previous, (int)cur);
    pending_item = -1;
    last_row = (int)cur;
    RECT rect;
    if (previous >= 0 && SendMessageW(list_hwnd, LB_GETITEMRECT, previous, (LPARAM)&rect) != LB_ERR)
        InvalidateRect(list_hwnd, &rect, FALSE);
    if (SendMessageW(list_hwnd, LB_GETITEMRECT, cur, (LPARAM)&rect) != LB_ERR)
        InvalidateRect(list_hwnd, &rect, FALSE);
    UpdateWindow(list_hwnd);
}

static void edit_key(WORD key) { SendMessageW(input_hwnd, WM_KEYDOWN, key, 0); }

static void move_caret(int pos)
{
    SendMessageW(input_hwnd, EM_SETSEL, pos, pos);
}

static bool at_start_unselected(HWND hwnd)
{
    DWORD start = 0, end = 0;
    SendMessageW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    return start == 0 && end == 0;
}

static void delete_to_eol(void)
{
    DWORD start = 0, endsel = 0;
    int end = GetWindowTextLengthW(input_hwnd);
    SendMessageW(input_hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&endsel);
    SendMessageW(input_hwnd, EM_SETSEL, start, end);
    edit_key(VK_DELETE);
}

static void delete_prev_word(void)
{
    DWORD start = 0, end = 0;
    int len = GetWindowTextLengthW(input_hwnd);
    wchar_t *s = (wchar_t *)calloc(len + 1, sizeof(wchar_t));
    if (!s) return;
    GetWindowTextW(input_hwnd, s, len + 1);
    SendMessageW(input_hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (start != end) {
        SendMessageW(input_hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
    } else if (start) {
        int i = (int)start;
        while (i > 0 && iswspace(s[i - 1])) --i;
        while (i > 0 && !iswspace(s[i - 1])) --i;
        SendMessageW(input_hwnd, EM_SETSEL, i, start);
        SendMessageW(input_hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
    }
    free(s);
}

struct key_state { bool ctrl, alt, shift; };

static struct key_state key_state(void)
{
    struct key_state k = {
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
    };
    return k;
}

static bool handle_global_keys(WORD key, struct key_state k)
{
    if (k.ctrl && key == 'O') { visit_location(-1); return true; }
    if (k.ctrl && key == 'I') { visit_location(1); return true; }
    if (k.ctrl && key == VK_RETURN && multi_select) { add_selection(); return true; }
    if (key == VK_RETURN) { finish(); return true; }
    if (key == VK_ESCAPE) { PostQuitMessage(0); return true; }
    if (key == VK_DOWN) { list_move(1); return true; }
    if (key == VK_UP) { list_move(-1); return true; }
    return false;
}

/* Detachable mode: remove this function and its call for plain Windows controls. */
static bool handle_emacs_keys(HWND hwnd, WORD key, struct key_state k)
{
    if (default_controls) return false;
    if (k.ctrl && key == VK_BACK) { delete_prev_word(); return true; }
    if (k.ctrl && key == 'A') { if (at_start_unselected(hwnd)) SendMessageW(hwnd, EM_SETSEL, 0, -1); else move_caret(0); return true; }
    if (k.ctrl && key == 'E') { move_caret(GetWindowTextLengthW(hwnd)); return true; }
    if (k.ctrl && key == 'K') { delete_to_eol(); return true; }
    if (k.ctrl && key == 'W') { SendMessageW(hwnd, WM_CUT, 0, 0); return true; }
    if (k.ctrl && k.alt && key == 'V') { vi_mode = !vi_mode; normal_mode = vi_mode; return true; }
    if (k.ctrl && key == 'N') { list_move(1); return true; }
    if (k.ctrl && key == 'P') { list_move(-1); return true; }
    if (k.ctrl && key == 'D') { list_move(lines > 0 ? lines : 5); return true; }
    if (k.ctrl && key == 'U') { list_move(-(lines > 0 ? lines : 5)); return true; }
    return false;
}

/* Detachable mode: remove this function and Ctrl+Alt+V toggle for no vi support. */
static bool handle_vi_keys(WORD key, struct key_state k)
{
    if (!vi_mode || !normal_mode) return false;
    switch (key) {
    case 'I': if (k.shift) edit_key(VK_HOME); normal_mode = false; return true;
    case 'A': if (k.shift) edit_key(VK_END); else edit_key(VK_RIGHT); normal_mode = false; return true;
    case 'H': edit_key(VK_LEFT); return true;
    case 'L': edit_key(VK_RIGHT); return true;
    case '0': edit_key(VK_HOME); return true;
    case '4': if (k.shift) { edit_key(VK_END); return true; } break;
    case 'X': if (k.shift) return true; edit_key(VK_DELETE); return true;
    case 'D': if (k.shift) { delete_to_eol(); return true; } break;
    case 'J': list_move(1); return true;
    case 'K': list_move(-1); return true;
    case 'G': {
        int target = k.shift ? (int)SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) - 1 : 0;
        if (target >= 0 && target < SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0)) {
            SendMessageW(list_hwnd, LB_SETCURSEL, target, 0);
            record_location(last_row, target);
            pending_item = -1;
            last_row = target;
            UpdateWindow(list_hwnd);
        }
        return true;
    }
    case 'W': edit_key(VK_RIGHT); return true;
    case 'B': edit_key(VK_LEFT); return true;
    }
    return false;
}

static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR && ((vi_mode && normal_mode) || wp == 0x7f ||
        ((GetKeyState(VK_CONTROL) & 0x8000) && (wp == 9 || wp == 15)))) return 0;
    if (msg == WM_KEYDOWN) {
        struct key_state k = key_state();
        if (handle_global_keys((WORD)wp, k)) return 0;
        if (handle_emacs_keys(hwnd, (WORD)wp, k)) return 0;
        if (handle_vi_keys((WORD)wp, k)) return 0;
    }
    return CallWindowProcW(old_input_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
        if (wp == VK_RETURN) { finish(); return 0; }
    }
    LRESULT r = CallWindowProcW(old_list_proc, hwnd, msg, wp, lp);
    if (msg == WM_SETFOCUS || msg == WM_LBUTTONDOWN || msg == WM_MOUSEWHEEL)
        SetFocus(input_hwnd);
    return r;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_STDIN_CHUNK:
        InterlockedExchange(&refresh_pending, 0);
        if (list_hwnd) {
            char *q = text_encoded(input_hwnd);
            if (q) refill_list(q);
            free(q);
        }
        return 0;
    case WM_STDIN_DONE:
        if (wp) { fputs("wmenu: failed to read stdin (I/O or memory error)\n", stderr); PostQuitMessage(1); return 0; }
        input_done = true;
        has_items = loaded_count() > 0;
        KillTimer(hwnd, TIMER_PROGRESS);
        if (list_hwnd) {
            char *q = text_encoded(input_hwnd);
            if (q) refill_list(q);
            free(q);
            update_progress();
        }
        return 0;
    case WM_SEARCH_STEP:
        search_posted = false;
        search_step();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_SEARCH) {
            KillTimer(hwnd, TIMER_SEARCH);
            if (!search_posted) search_step();
            return 0;
        }
        if (wp == TIMER_PROGRESS && progress_hwnd && !input_done) {
            if (list_hwnd) refill_list(search_query ? search_query : "");
            progress_pos += progress_dir;
            if (progress_pos >= 100 || progress_pos <= 0) progress_dir = -progress_dir;
            SendMessageW(progress_hwnd, PBM_SETPOS, progress_pos, 0);
        }
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *measure = (MEASUREITEMSTRUCT *)lp;
        if (measure->CtlID != ID_COMBO) break;
        HDC dc = GetDC(hwnd);
        HFONT old = (HFONT)SelectObject(dc, ui_font);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        measure->itemHeight = tm.tmHeight + S(4);
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *draw = (DRAWITEMSTRUCT *)lp;
        if (draw->CtlID != ID_COMBO || draw->itemID == (UINT)-1) break;
        bool selected_row = (int)draw->itemID == (int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0);
        FillRect(draw->hDC, &draw->rcItem,
                 GetSysColorBrush(selected_row ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        EnterCriticalSection(&items_lock);
        int index = row_item((int)draw->itemID);
        wchar_t *w = index >= 0 && index < item_count ? widen(items[index]) : NULL;
        LeaveCriticalSection(&items_lock);
        if (w) {
            HFONT old = (HFONT)SelectObject(draw->hDC, ui_font);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, GetSysColor(selected_row ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            RECT rect = draw->rcItem;
            rect.left += S(4);
            DrawTextW(draw->hDC, w, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(draw->hDC, old);
            free(w);
        }
        if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_LABEL) return 0;
        if (LOWORD(wp) == ID_EDIT && HIWORD(wp) == EN_CHANGE && has_items) {
            char *q = text_encoded(input_hwnd);
            refill_list(q);
            free(q);
        }
        if (LOWORD(wp) == ID_COMBO && HIWORD(wp) == LBN_SELCHANGE) {
            int row = (int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0);
            record_location(last_row, row);
            pending_item = -1;
            last_row = row;
        }
        if (LOWORD(wp) == ID_COMBO && HIWORD(wp) == LBN_DBLCLK) {
            if (multi_select) add_selection(); else finish();
        }
        return 0;
    case WM_ACTIVATE:
        if (close_on_blur && LOWORD(wp) == WA_INACTIVE) PostQuitMessage(0);
        else if (LOWORD(wp) != WA_INACTIVE && input_hwnd) SetFocus(input_hwnd);
        return 0;
    case WM_ACTIVATEAPP:
        if (close_on_blur && !wp) PostQuitMessage(0);
        else if (wp && input_hwnd) SetFocus(input_hwnd);
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void usage(void)
{
    puts("usage: wmenu [-chimbdVv] [-e utf8|ansi|oem] [-p prompt] [-x px] [-y px] [-w px] [-l lines] [-fn name] [-fs pt] [-fw weight] < items");
}

int main(int argc, char **argv)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    WNDCLASSW wc = {0};
    MSG msg;
    int prompt_w = 0;

    ARGBEGIN {
    case 'h':
        usage();
        return 0;
    case 'v':
        puts("version: 0.8");
        return 0;
    case 'e': {
        char *enc = EARGF(usage());
        if (!strcmp(enc, "ansi")) text_codepage = CP_ACP;
        else if (!strcmp(enc, "oem")) text_codepage = CP_OEMCP;
        else if (!strcmp(enc, "utf8") || !strcmp(enc, "utf-8")) text_codepage = CP_UTF8;
    } break;
    case 'p': {
        wchar_t *w = widen(EARGF(usage()));
        if (!w) { fputs("wmenu: out of memory reading prompt\n", stderr); return 1; }
        has_prompt = true;
        wcsncpy(prompt, w, 299);
        prompt[299] = 0;
        free(w);
    } break;
    case 'f': {
        wchar_t *w;
        if (argv[0][1] == 'n') {
            argv[0]++;
            w = widen(EARGF(usage()));
            if (!w) { fputs("wmenu: out of memory reading font name\n", stderr); return 1; }
            custom_font = true;
            wcsncpy(font_name, w, LF_FACESIZE - 1);
            font_name[LF_FACESIZE - 1] = 0;
            free(w);
        } else if (argv[0][1] == 's') {
            argv[0]++;
            custom_font = true;
            font_size = atoi(EARGF(usage()));
        } else if (argv[0][1] == 'w') {
            argv[0]++;
            custom_font = true;
            font_weight = atoi(EARGF(usage()));
        }
    } break;
    case 'x': win_x = atoi(EARGF(usage())); break;
    case 'y': win_y = atoi(EARGF(usage())); break;
    case 'w': win_w = atoi(EARGF(usage())); break;
    case 'l': lines = atoi(EARGF(usage())); break;
    case 'c': center_window = true; break;
    case 'm': multi_select = true; break;
    case 'V': vi_mode = true; break;
    case 'd': default_controls = true; break;
    case 'b': close_on_blur = true; break;
    case 'i': break;
    default: usage(); return 1;
    } ARGEND;

    InitializeCriticalSection(&items_lock);
    stdin_pipe = !_isatty(_fileno(stdin));
    input_done = !stdin_pipe;
    if (stdin_pipe) { has_items = true; loading_items = true; }

    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    InitCommonControlsEx(&icc);
    make_font();
    if (!win_w) win_w = GetSystemMetrics(SM_CXSCREEN);
    win_h = has_items ? S(28) + (lines > 0 ? lines : 5) * S(22) : S(28);
    if (center_window) {
        win_x = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
        win_y = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;
    }

    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"wmenu";
    if (!RegisterClassW(&wc)) return 1;

    main_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, L"wmenu", L"wmenu", WS_POPUP | WS_BORDER, win_x, win_y, win_w, win_h, NULL, NULL, inst, NULL);
    if (!main_hwnd) return 1;

    if (has_prompt) {
        prompt_w = (int)wcslen(prompt) * S(8) + S(12);
        HWND label = CreateWindowW(L"STATIC", prompt, WS_CHILD | WS_VISIBLE, S(4), S(5), prompt_w, S(22), main_hwnd, (HMENU)ID_LABEL, inst, NULL);
        set_font(label);
    }

    if (has_items) {
        input_hwnd = CreateWindowW(WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, prompt_w, S(2), win_w - prompt_w, S(24), main_hwnd, (HMENU)ID_EDIT, inst, NULL);
        set_font(input_hwnd);
        list_hwnd = CreateWindowW(WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | LBS_NODATA | LBS_OWNERDRAWFIXED | WS_VSCROLL, 0, S(28), win_w, win_h - S(28) - (loading_items ? S(10) : 0), main_hwnd, (HMENU)ID_COMBO, inst, NULL);
        set_font(list_hwnd);
        old_list_proc = (WNDPROC)SetWindowLongPtrW(list_hwnd, GWLP_WNDPROC, (LONG_PTR)ListProc);
        progress_hwnd = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | (loading_items ? WS_VISIBLE : 0), 0, win_h - S(10), win_w, S(10), main_hwnd, (HMENU)ID_PROGRESS, inst, NULL);
        progress_visible = loading_items;
        SendMessageW(progress_hwnd, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        if (!loading_items) refill_list("");
    } else {
        input_hwnd = CreateWindowW(WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, prompt_w, S(2), win_w - prompt_w, S(24), main_hwnd, (HMENU)ID_EDIT, inst, NULL);
        set_font(input_hwnd);
    }
    old_input_proc = (WNDPROC)SetWindowLongPtrW(input_hwnd, GWLP_WNDPROC, (LONG_PTR)InputProc);
    if (has_items) {
        ui_fiber = ConvertThreadToFiber(NULL);
        if (!ui_fiber) return 1;
        search_fiber = CreateFiber(0, search_fiber_run, NULL);
        if (!search_fiber) { ConvertFiberToThread(); return 1; }
    }

    ShowWindow(main_hwnd, SW_SHOW);
    UpdateWindow(main_hwnd);
    SetFocus(input_hwnd);
    if (loading_items) {
        SetTimer(main_hwnd, TIMER_PROGRESS, 30, NULL);
        HANDLE reader = CreateThread(NULL, 0, read_stdin_thread, NULL, 0, NULL);
        if (!reader) { fputs("wmenu: cannot start stdin reader\n", stderr); return 1; }
        CloseHandle(reader);
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (search_fiber) {
        DeleteFiber(search_fiber);
        ConvertFiberToThread();
    }
    return (int)msg.wParam;
}
