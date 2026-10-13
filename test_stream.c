#define main wmenu_main
#include "wmenu.c"
#undef main
#include <assert.h>

int main(void)
{
    InitializeCriticalSection(&items_lock);
    main_hwnd = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, NULL, NULL);
    list_hwnd = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VSCROLL, 0, 0, 400, 300, main_hwnd, NULL, NULL, NULL);
    assert(list_hwnd);
    for (int i = 0; i < 10; ++i) {
        char buf[32];
        sprintf(buf, "item-%d", i);
        add_item(buf);
    }
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == 10);
    SendMessageW(list_hwnd, LB_SETCURSEL, 5, 0);
    for (int i = 10; i < LIST_LIMIT + 10; ++i) {
        char buf[32];
        sprintf(buf, "item-%d", i);
        add_item(buf);
    }
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == LIST_LIMIT);
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 5);
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCURSEL, 0, 0) == 5);
    refill_list("item-9");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) > 0);
    refill_list("");
    assert(SendMessageW(list_hwnd, LB_GETCOUNT, 0, 0) == LIST_LIMIT);
    DestroyWindow(main_hwnd);
    DeleteCriticalSection(&items_lock);
    return 0;
}
