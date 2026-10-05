#include <windows.h>

static char g_typed[64];
static int g_n;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        HBRUSH bg;
        HPEN pen;
        HGDIOBJ old;
        GetClientRect(h, &rc);
        bg = CreateSolidBrush(RGB(20, 28, 40));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 240, 245));
        TextOutA(dc, 14, 12, "CoreZ Win32 GDI", 19);
        SetTextColor(dc, RGB(130, 220, 170));
        TextOutA(dc, 14, 34, "keys:", 5);
        if (g_n > 0)
            TextOutA(dc, 56, 34, g_typed, g_n);
        pen = CreatePen(0, 2, RGB(255, 128, 128));
        old = SelectObject(dc, pen);
        Rectangle(dc, 10, 58, rc.right - 10, rc.bottom - 10);
        pen = CreatePen(0, 1, RGB(120, 180, 255));
        SelectObject(dc, pen);
        DeleteObject(old);
        Ellipse(dc, 28, 78, 152, 156);
        MoveToEx(dc, 28, 156, NULL);
        LineTo(dc, 152, 78);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CHAR:
        if (g_n < (int)sizeof(g_typed) - 1) {
            g_typed[g_n++] = (char)(wp & 0x7F);
            g_typed[g_n] = 0;
        }
        InvalidateRect(h, NULL, 0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int main(void) {
    WNDCLASSA wc;
    HWND h;
    MSG msg;

    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = NULL;
    wc.hIcon = NULL;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "WinGDI32 Test";
    if (!RegisterClassA(&wc))
        return 1;

    h = CreateWindowExA(0, "WinGDI32 Test", "win32 gdi demo", WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, 420, 260, NULL, NULL, NULL,
                        NULL);
    if (!h)
        return 2;

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}