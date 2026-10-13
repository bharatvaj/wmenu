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
    assert(has_match("aC", "ABC"));
    assert(!has_match("Ab", "abc")); /* Preserve fzy's ASCII query casing. */
    assert(has_match("\xc3\xa9", "caf\xc3\xa9"));
    assert(!has_match("\xc3\xa9", "caf\xc3\x89"));
    FILE *input = tmpfile();
    assert(input);
    char long_row[8193];
    memset(long_row, 'x', sizeof long_row - 1);
    long_row[sizeof long_row - 1] = 0;
    assert(fputs(long_row, input) >= 0);
    assert(fputs("\r\nnext\r\npartial", input) >= 0);
    rewind(input);
    char *line = NULL;
    size_t capacity = 0;
    assert(read_line(input, &line, &capacity) == 1);
    assert(!strcmp(line, long_row));
    assert(read_line(input, &line, &capacity) == 1 && !strcmp(line, "next"));
    assert(read_line(input, &line, &capacity) == 1 && !strcmp(line, "partial"));
    assert(read_line(input, &line, &capacity) == 0);
    free(line);
    fclose(input);

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
    input_hwnd = CreateWindowW(L"EDIT", L"", WS_CHILD, 0, 0, 400, 28, main_hwnd, (HMENU)ID_EDIT, wc.hInstance, NULL);
    assert(list_hwnd && progress_hwnd && input_hwnd);
    ui_fiber = ConvertThreadToFiber(NULL);
    assert(ui_fiber);
    search_fiber = CreateFiber(0, search_fiber_run, NULL);
    assert(search_fiber);
    bool stress = argc > 1 && !strcmp(argv[1], "stress");
    ShowWindow(main_hwnd, SW_SHOWNOACTIVATE);
    ShowWindow(list_hwnd, SW_SHOWNOACTIVATE);
    assert(add_item("item-0"));
    int saved_capacity = item_capacity;
    char **saved_items = items;
    item_count = item_capacity = INT_MAX;
    assert(!add_item("overflow"));
    assert(item_count == INT_MAX && items == saved_items);
    item_count = 1;
    item_capacity = saved_capacity;
    WndProc(main_hwnd, WM_STDIN_CHUNK, 0, 0);
    UpdateWindow(list_hwnd);
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 1);
    assert(draw_calls > 0); /* First streamed row must actually paint. */
    vi_mode = true;
    normal_mode = false;
    assert(InputProc(input_hwnd, WM_KEYDOWN, VK_ESCAPE, 0) == 0 && normal_mode);
    MSG quit;
    assert(!PeekMessageW(&quit, NULL, WM_QUIT, WM_QUIT, PM_NOREMOVE));
    normal_mode = false;
    assert(ListProc(list_hwnd, WM_KEYDOWN, VK_ESCAPE, 0) == 0 && normal_mode);
    assert(!PeekMessageW(&quit, NULL, WM_QUIT, WM_QUIT, PM_NOREMOVE));
    vi_mode = normal_mode = false;
    for (int i = 1; i < 60000; ++i) {
        char buf[2700];
        sprintf(buf, "item-%d", i);
        if (stress) {
            size_t len = strlen(buf);
            memset(buf + len, 'x', 2699 - len);
            buf[2699] = 0; /* About 155 MiB of line data in stress mode. */
        }
        assert(add_item(buf));
    }
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 60000);
    SendMessageW(list_hwnd, LB_SETCURSEL, 0, 0);
    UpdateWindow(list_hwnd);
    visit_location(-1); /* Empty history and both ends are no-ops. */
    visit_location(1);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    list_move(5);
    list_move(5);
    struct key_state ctrl = { true, false, false };
    assert(handle_global_keys('O', ctrl));
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 5);
    visit_location(-1);
    visit_location(-1);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    assert(handle_global_keys('I', ctrl));
    visit_location(1);
    visit_location(1);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 10);
    visit_location(-1);
    list_move(2);
    visit_location(1); /* A new move discards the old forward branch. */
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 7);
    SendMessageW(list_hwnd, LB_SETCURSEL, 9, 0);
    WndProc(main_hwnd, WM_COMMAND, MAKEWPARAM(ID_COMBO, LBN_SELCHANGE), (LPARAM)list_hwnd);
    visit_location(-1);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 7);
    location_count = location_cursor = 0;
    for (int i = 0; i < LOCATION_LIMIT + 2; ++i) record_location(i, i + 1);
    assert(location_count == LOCATION_LIMIT && location_cursor == LOCATION_LIMIT - 1);
    assert(locations[0].item == 3); /* Oldest positions drop off at the limit. */
    for (int i = 0; i < location_count; ++i) free(locations[i].query);
    location_count = location_cursor = 0;
    list_move(-2);
    list_move(5);
    assert(add_item("extra-row"));
    input_done = false;
    WndProc(main_hwnd, WM_TIMER, TIMER_PROGRESS, 0); /* Partial batch before EOF. */
    input_done = true;
    update_progress();
    UpdateWindow(list_hwnd);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 10);
    assert(SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0) == 0);
    assert(!painted_selected[5] && painted_selected[10]);
    list_move(-5);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 5);
    assert(painted_selected[5] && !painted_selected[10]);
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
    assert(location_count > 0); /* Previous search is retained. */
    int searches = location_count;
    refill_list("item-12");
    refill_list("item-123");
    refill_list("item-1");
    assert(location_count == searches); /* Editing one query is not four jumps. */
    while (search_pos < 1024) search_step();
    int old_item = matches[match_count - 1];
    SendMessageW(list_hwnd, LB_SETCURSEL, match_count - 1, 0);
    assert(SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0) > 0);
    SetWindowTextW(input_hwnd, L"item-1");
    SetWindowTextW(input_hwnd, L""); /* EN_CHANGE: clearing returns to the top. */
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    assert(SendMessageW(list_hwnd, LB_GETTOPINDEX, 0, 0) == 0);
    visit_location(-1);
    assert(!strcmp(search_query, "item-1"));
    search_step();
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == LB_ERR);
    char *pending_text = current_text(); /* Enter must use the saved line, not row zero. */
    assert(pending_text && !strcmp(pending_text, items[old_item]));
    free(pending_text);
    while (search_pos <= old_item) search_step();
    assert(row_item((int)SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0)) == old_item);
    visit_location(1);
    assert(!strcmp(search_query, ""));
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    visit_location(-1);
    search_step();
    list_move(1); /* Manual navigation cancels a pending history restore. */
    assert(pending_item == -1 && SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 0);
    visit_location(-1);
    assert(pending_item == old_item && SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == LB_ERR);
    refill_list("");
    refill_list("item-1");
    search_step();
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
    assert(add_item("item-60000"));
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
    assert(add_item("tail-special"));
    while (search_pos < item_count) search_step();
    assert(match_count == 1 && matches[0] == 60002);
    input_done = true;
    update_progress();
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 1);
    assert(add_item("caf\xc3\xa9"));
    SetWindowTextW(input_hwnd, L"\x00e9"); /* UTF-8 edit text goes through the search path. */
    while (search_pos < loaded_count()) search_step();
    assert(match_count == 1 && matches[0] == 60003);
    DeleteFiber(search_fiber);
    ConvertFiberToThread();
    DestroyWindow(main_hwnd);
    DeleteCriticalSection(&items_lock);
    return 0;
}
