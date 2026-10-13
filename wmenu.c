#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <wctype.h>

#include "deps/fzy/src/match.h"

#ifdef _MSC_VER
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define ID_COMBO 101
#define ID_EDIT 102
#define ID_LABEL 103
#define ID_PROGRESS 104
#define WM_STDIN_CHUNK (WM_APP + 1)
#define WM_STDIN_DONE (WM_APP + 2)

static int dpi = 96;
#define S(px) MulDiv((px), dpi, 96)

static char **items, **selected;
static int item_count, selected_count;
static bool has_items, has_prompt, multi_select, vi_mode, normal_mode, default_controls;
static wchar_t prompt[300];
static HWND main_hwnd, input_hwnd, list_hwnd, progress_hwnd;
static WNDPROC old_input_proc, old_list_proc;
static CRITICAL_SECTION items_lock;
static int win_x = 0, win_y = 0, win_w = 0, win_h = 0, lines = 5;
static int font_size = 0, font_weight = FW_NORMAL;
static UINT text_codepage = CP_UTF8;
static wchar_t font_name[LF_FACESIZE];
static HFONT ui_font;
static bool custom_font;
static bool center_window, close_on_blur, stdin_pipe, loading_items;

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

static void add_item(const char *s)
{
    EnterCriticalSection(&items_lock);
    items = (char **)realloc(items, sizeof(char *) * (item_count + 1));
    items[item_count++] = _strdup(s ? s : "");
    LeaveCriticalSection(&items_lock);
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

static DWORD WINAPI read_stdin_thread(void *unused)
{
    char buf[4096];
    int n = 0;
    (void)unused;
    while (fgets(buf, sizeof buf, stdin)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (*buf) {
            add_item(buf);
            if (++n == 1 || n % 16 == 0) PostMessageW(main_hwnd, WM_STDIN_CHUNK, 0, 0);
        }
    }
    loading_items = false;
    PostMessageW(main_hwnd, WM_STDIN_DONE, 0, 0);
    return 0;
}

struct ranked_item { int index; score_t score; };

static int rank_cmp(const void *a, const void *b)
{
    const struct ranked_item *x = (const struct ranked_item *)a, *y = (const struct ranked_item *)b;
    return x->score < y->score ? 1 : x->score > y->score ? -1 : x->index - y->index;
}

static void refill_list(const char *query)
{
    struct ranked_item *ranked;
    int n = 0;
    EnterCriticalSection(&items_lock);
    ranked = (struct ranked_item *)calloc(item_count, sizeof(*ranked));
    SendMessageW(list_hwnd, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < item_count; ++i) {
        if (!query || !*query || has_match(query, items[i])) {
            ranked[n].index = i;
            ranked[n++].score = query && *query ? match(query, items[i]) : 0;
        }
    }
    qsort(ranked, n, sizeof(*ranked), rank_cmp);
    for (int i = 0; i < n; ++i) {
        wchar_t *w = widen(items[ranked[i].index]);
        SendMessageW(list_hwnd, LB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    LeaveCriticalSection(&items_lock);
    if (n) SendMessageW(list_hwnd, LB_SETCURSEL, 0, 0);
    free(ranked);
}

static char *current_text(void)
{
    wchar_t *w;
    int n, m;
    char *s;
    LRESULT sel = list_hwnd ? SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) : LB_ERR;
    if (has_items && sel != LB_ERR) {
        n = (int)SendMessageW(list_hwnd, LB_GETTEXTLEN, sel, 0) + 1;
        w = (wchar_t *)calloc(n, sizeof(wchar_t));
        if (!w) return NULL;
        SendMessageW(list_hwnd, LB_GETTEXT, sel, (LPARAM)w);
    } else {
        n = GetWindowTextLengthW(input_hwnd) + 1;
        w = (wchar_t *)calloc(n, sizeof(wchar_t));
        if (!w) return NULL;
        GetWindowTextW(input_hwnd, w, n);
    }
    m = WideCharToMultiByte(text_codepage, 0, w, -1, NULL, 0, NULL, NULL);
    s = (char *)calloc(m ? m : 1, 1);
    if (s) WideCharToMultiByte(text_codepage, 0, w, -1, s, m, NULL, NULL);
    free(w);
    return s;
}

static void add_selection(void)
{
    char *s = current_text();
    if (!s || !*s) { free(s); return; }
    selected = (char **)realloc(selected, sizeof(char *) * (selected_count + 1));
    selected[selected_count++] = s;
    SetWindowTextW(input_hwnd, L"");
    if (has_items) refill_list("");
}

static void finish(void)
{
    for (int i = 0; i < selected_count; ++i) printf("%s\n", selected[i]);
    if (input_hwnd) {
        char *s = current_text();
        if (s && *s) fputs(s, stdout);
        free(s);
    }
    PostQuitMessage(0);
}

static void list_move(int delta)
{
    if (!has_items) return;
    LRESULT count = SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0);
    LRESULT cur = SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0);
    if (count <= 0) return;
    if (cur < 0) cur = delta > 0 ? -1 : count;
    cur += delta;
    if (cur < 0) cur = 0;
    if (cur >= count) cur = count - 1;
    SendMessageW(list_hwnd, LB_SETCURSEL, cur, 0);
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
    case 'G': SendMessageW(list_hwnd, LB_SETCURSEL, k.shift ? SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) - 1 : 0, 0); return true;
    case 'W': edit_key(VK_RIGHT); return true;
    case 'B': edit_key(VK_LEFT); return true;
    }
    return false;
}

static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR && ((vi_mode && normal_mode) || wp == 0x7f)) return 0;
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
        if (list_hwnd && loading_items) {
            char *q = text_encoded(input_hwnd);
            refill_list(q);
            free(q);
        }
        return 0;
    case WM_STDIN_DONE:
        has_items = item_count > 0;
        if (progress_hwnd) ShowWindow(progress_hwnd, SW_HIDE);
        if (list_hwnd) {
            SetWindowPos(list_hwnd, NULL, 0, S(28), win_w, win_h - S(28), SWP_NOZORDER);
            refill_list("");
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_LABEL) return 0;
        if (LOWORD(wp) == ID_EDIT && HIWORD(wp) == EN_CHANGE && has_items && !loading_items) {
            char *q = text_encoded(input_hwnd);
            refill_list(q);
            free(q);
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
    puts("usage: wmenu [-p prompt] [-enc utf8|ansi|oem] [-fn name] [-fs pt] [-fw weight] [-x px] [-y px] [-w px] [-c] [-l lines] [-multi] [-vi] [-default-controls] [-close-on-blur] < items");
}

int main(int argc, char **argv)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    WNDCLASSW wc = {0};
    MSG msg;
    int prompt_w = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-help")) { usage(); return 0; }
        if (!strcmp(argv[i], "-version")) { puts("version: 0.7"); return 0; }
        if ((!strcmp(argv[i], "-enc") || !strcmp(argv[i], "-encoding")) && i + 1 < argc) {
            char *enc = argv[++i];
            if (!strcmp(enc, "ansi")) text_codepage = CP_ACP;
            else if (!strcmp(enc, "oem")) text_codepage = CP_OEMCP;
            else if (!strcmp(enc, "utf8") || !strcmp(enc, "utf-8")) text_codepage = CP_UTF8;
        }
        else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "-prompt")) && i + 1 < argc) { wchar_t *w = widen(argv[++i]); has_prompt = true; wcsncpy(prompt, w, 299); prompt[299] = 0; free(w); }
        else if ((!strcmp(argv[i], "-fn") || !strcmp(argv[i], "-font")) && i + 1 < argc) { wchar_t *w = widen(argv[++i]); custom_font = true; wcsncpy(font_name, w, LF_FACESIZE - 1); font_name[LF_FACESIZE - 1] = 0; free(w); }
        else if ((!strcmp(argv[i], "-fs") || !strcmp(argv[i], "-font-size")) && i + 1 < argc) { custom_font = true; font_size = atoi(argv[++i]); }
        else if ((!strcmp(argv[i], "-fw") || !strcmp(argv[i], "-font-weight")) && i + 1 < argc) { custom_font = true; font_weight = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) win_x = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y") && i + 1 < argc) win_y = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) win_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) lines = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-center")) center_window = true;
        else if (!strcmp(argv[i], "-multi")) multi_select = true;
        else if (!strcmp(argv[i], "-vi")) vi_mode = true;
        else if (!strcmp(argv[i], "-default-controls")) default_controls = true;
        else if (!strcmp(argv[i], "-close-on-blur")) close_on_blur = true;
        else if (!strcmp(argv[i], "-i")) ;
    }

    InitializeCriticalSection(&items_lock);
    stdin_pipe = !_isatty(_fileno(stdin));
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
        list_hwnd = CreateWindowW(WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 0, S(28), win_w, win_h - S(28) - (loading_items ? S(6) : 0), main_hwnd, (HMENU)ID_COMBO, inst, NULL);
        set_font(list_hwnd);
        old_list_proc = (WNDPROC)SetWindowLongPtrW(list_hwnd, GWLP_WNDPROC, (LONG_PTR)ListProc);
        progress_hwnd = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | (loading_items ? WS_VISIBLE : 0) | PBS_MARQUEE, 0, win_h - S(6), win_w, S(6), main_hwnd, (HMENU)ID_PROGRESS, inst, NULL);
        SendMessageW(progress_hwnd, PBM_SETMARQUEE, TRUE, 30);
        if (!loading_items) refill_list("");
    } else {
        input_hwnd = CreateWindowW(WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, prompt_w, S(2), win_w - prompt_w, S(24), main_hwnd, (HMENU)ID_EDIT, inst, NULL);
        set_font(input_hwnd);
    }
    old_input_proc = (WNDPROC)SetWindowLongPtrW(input_hwnd, GWLP_WNDPROC, (LONG_PTR)InputProc);

    ShowWindow(main_hwnd, SW_SHOW);
    UpdateWindow(main_hwnd);
    SetFocus(input_hwnd);
    if (loading_items) CreateThread(NULL, 0, read_stdin_thread, NULL, 0, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
