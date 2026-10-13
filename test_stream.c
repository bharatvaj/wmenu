#define main wmenu_main
#include "wmenu.c"
#undef main
#include <assert.h>

static int draw_calls;
static bool painted_selected[16];
static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DRAWITEM) {
        DRAWITEMSTRUCT *draw = (DRAWITEMSTRUCT *)lp;
        ++draw_calls;
        if (draw->itemID < 16) painted_selected[draw->itemID] = (draw->itemState & ODS_SELECTED) != 0;
    }
    return WndProc(hwnd, msg, wp, lp);
}

int main(int argc, char **argv)
{
    InitializeCriticalSection(&items_lock);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    assert(InitCommonControlsEx(&icc));
    make_font();
    win_w = 400;
    win_h = 300;
    input_done = true;
    has_items = true;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"wmenu-test";
    assert(RegisterClassW(&wc));
    main_hwnd = CreateWindowW(L"wmenu-test", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
    list_hwnd = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VSCROLL | LBS_NODATA | LBS_OWNERDRAWFIXED,
                              0, 0, 400, 300, main_hwnd, (HMENU)ID_COMBO, wc.hInstance, NULL);
    progress_hwnd = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD, 0, 290, 400, 10, main_hwnd, NULL, NULL, NULL);
    input_hwnd = CreateWindowW(L"EDIT", L"", WS_CHILD, 0, 0, 400, 28, main_hwnd, NULL, NULL, NULL);
    assert(list_hwnd && progress_hwnd && input_hwnd);
    ui_fiber = ConvertThreadToFiber(NULL);
    assert(ui_fiber);
    search_fiber = CreateFiber(0, search_fiber_run, NULL);
    assert(search_fiber);
    bool stress = argc > 1 && !strcmp(argv[1], "stress");
    ShowWindow(main_hwnd, SW_SHOWNOACTIVATE);
    ShowWindow(list_hwnd, SW_SHOWNOACTIVATE);
    add_item("item-0");
    WndProc(main_hwnd, WM_STDIN_CHUNK, 0, 0);
    UpdateWindow(list_hwnd);
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 1);
    assert(draw_calls > 0); /* First streamed row must actually paint. */
    for (int i = 1; i < 60000; ++i) {
        char buf[2700];
        sprintf(buf, "item-%d", i);
        if (stress) {
            size_t len = strlen(buf);
            memset(buf + len, 'x', 2699 - len);
            buf[2699] = 0; /* About 155 MiB of line data in stress mode. */
        }
        add_item(buf);
    }
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 60000);
    SendMessageW(list_hwnd, LB_SETCURSEL, 0, 0);
    UpdateWindow(list_hwnd);
    list_move(5);
    add_item("extra-row");
    refill_list(""); /* Count update before the selection's WM_PAINT. */
    UpdateWindow(list_hwnd);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 5);
    assert(SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0) == 0);
    assert(!painted_selected[0] && painted_selected[5]);
    list_move(-5);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    assert(painted_selected[0] && !painted_selected[5]);
    SendMessageW(list_hwnd, LB_SETCURSEL, 59999, 0);
    char *last = current_text();
    assert(last && !strncmp(last, "item-59999", 10));
    free(last);
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 59999);
    refill_list("item-1");
    search_step();
    assert(search_pos == SEARCH_BATCH);
    refill_list("item-59999"); /* Cancel the previous search and start over. */
    assert(progress_visible);
    while (search_pos < item_count) search_step();
    assert(match_count == 1 && matches[0] == 59999);
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 1);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    last = current_text();
    assert(last && !strncmp(last, "item-59999", 10));
    free(last);
    assert(!progress_visible);
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 250, 30);
    HGDIOBJ old = SelectObject(dc, bitmap);
    DRAWITEMSTRUCT draw = {0};
    draw.CtlID = ID_COMBO;
    draw.itemID = 0;
    draw.itemState = ODS_SELECTED;
    draw.hDC = dc;
    draw.rcItem.right = 250;
    draw.rcItem.bottom = 30;
    assert(WndProc(main_hwnd, WM_DRAWITEM, ID_COMBO, (LPARAM)&draw) == TRUE);
    assert(GetPixel(dc, 1, 1) == GetSysColor(COLOR_HIGHLIGHT));
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 60001);
    ShowWindow(main_hwnd, SW_SHOWNOACTIVATE);
    ShowWindow(list_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(list_hwnd);
    assert(!GetUpdateRect(list_hwnd, NULL, FALSE));
    SendMessageW(list_hwnd, LB_SETCURSEL, 59999, 0);
    int kept_top = (int)SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0);
    add_item("item-60000");
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 60002);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 59999);
    assert(SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0) == kept_top);
    RECT dirty;
    assert(!GetUpdateRect(list_hwnd, &dirty, FALSE) || IsRectEmpty(&dirty));
    /* Count/scrollbar may update, but the existing client rows stay untouched. */
    refill_list("tail-special");
    search_step();
    input_done = false;
    add_item("tail-special");
    while (search_pos < item_count) search_step();
    assert(match_count == 1 && matches[0] == 60002);
    input_done = true;
    update_progress();
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 1);
    DeleteFiber(search_fiber);
    ConvertFiberToThread();
    DestroyWindow(main_hwnd);
    DeleteCriticalSection(&items_lock);
    return 0;
}
