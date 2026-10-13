#define main wmenu_main
#include "wmenu.c"
#undef main
#include <assert.h>

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
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"wmenu-test";
    assert(RegisterClassW(&wc));
    main_hwnd = CreateWindowW(L"wmenu-test", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
    list_hwnd = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VSCROLL | LBS_NODATA | LBS_OWNERDRAWFIXED,
                              0, 0, 400, 300, main_hwnd, (HMENU)ID_COMBO, wc.hInstance, NULL);
    progress_hwnd = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD, 0, 290, 400, 10, main_hwnd, NULL, NULL, NULL);
    assert(list_hwnd && progress_hwnd);
    ui_fiber = ConvertThreadToFiber(NULL);
    assert(ui_fiber);
    search_fiber = CreateFiber(0, search_fiber_run, NULL);
    assert(search_fiber);
    bool stress = argc > 1 && !strcmp(argv[1], "stress");
    for (int i = 0; i < 60000; ++i) {
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
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 60000);
    refill_list("tail-special");
    search_step();
    input_done = false;
    add_item("tail-special");
    while (search_pos < item_count) search_step();
    assert(match_count == 1 && matches[0] == 60000);
    input_done = true;
    update_progress();
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 1);
    DeleteFiber(search_fiber);
    ConvertFiberToThread();
    DestroyWindow(main_hwnd);
    DeleteCriticalSection(&items_lock);
    return 0;
}
