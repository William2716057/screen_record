




//g++ -O2 -std=c++17 -mwindows -municode script2.cpp -o delete.exe -lgdi32 -luser32 -lwinmm
// Requires ffmpeg.exe on PATH.
 
#include <windows.h>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cwchar>
 
// Encode settings and the selection box's starting size/position (you can
// drag it anywhere and resize it afterwards — these are just defaults).

constexpr int FPS               = 60;
constexpr int MAX_SECONDS       = 60;     //auto-stop even if Stop isn't clicked
constexpr int DEFAULT_SEL_W     = 640;
constexpr int DEFAULT_SEL_H     = 360;
constexpr int MIN_SEL_W         = 40;     // smallest resize
constexpr int MIN_SEL_H         = 40;
constexpr int HANDLE_SIZE       = 10;     // edges
const wchar_t* OUT_FILE         = L"output.mp4"; //change to user input
constexpr COLORREF OVERLAY_KEY  = RGB(255, 0, 255);   // "transparent" colours
constexpr COLORREF BORDER_COLOR = RGB(255, 90, 0);
constexpr COLORREF HANDLE_COLOR = RGB(255, 255, 255);
 

static std::atomic<bool>      g_recording{false};
static std::atomic<bool>      g_stopRequested{false};
static std::atomic<long long> g_framesWritten{0};
static std::thread            g_recordThread;
 
static HWND g_hwnd = nullptr, g_btnRecord = nullptr, g_btnStop = nullptr, g_lblStatus = nullptr;
static HWND g_overlay = nullptr;
 
constexpr int  IDC_RECORD          = 101;
constexpr int  IDC_STOP            = 102;
constexpr UINT WM_RECORDING_DONE   = WM_APP + 1;   // wParam: 1=ok/0=failed, lParam: frame count
constexpr UINT WM_SELECTION_CHANGED = WM_APP + 2;  // sent by the overlay whenever its rect changes
constexpr UINT_PTR TIMER_ID_UI     = 1;
 
static void enableDpiAwareness()
{
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (setCtx && setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
            return;
    }
    SetProcessDPIAware();
}
 
struct ScreenCapture {
    HDC     screenDC = nullptr;
    HDC     memDC    = nullptr;
    HBITMAP dib      = nullptr;
    HGDIOBJ oldBmp   = nullptr;
    void*   pixels   = nullptr;   // BGRA, top-down, width*height*4 bytes
    int     width = 0, height = 0;
    int     originX = 0, originY = 0;
 
    bool init(int x, int y, int w, int h)
    {
        originX = x; originY = y; width = w; height = h;
        if (width <= 0 || height <= 0) return false;
 
        screenDC = GetDC(nullptr);
        if (!screenDC) return false;
 
        memDC = CreateCompatibleDC(screenDC);
        if (!memDC) return false;
 
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = width;
        bmi.bmiHeader.biHeight      = -height;      // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
 
        dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dib || !pixels) return false;
 
        oldBmp = SelectObject(memDC, dib);
        return true;
    }
 
    void grab()
    {
        BitBlt(memDC, 0, 0, width, height, screenDC, originX, originY, SRCCOPY | CAPTUREBLT);
        drawCursor();
    }
 
    void drawCursor()
    {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
            return;
 
        ICONINFO ii{};
        if (!GetIconInfo(ci.hCursor, &ii)) return;
 
        DrawIconEx(memDC,
                   ci.ptScreenPos.x - originX - static_cast<int>(ii.xHotspot),
                   ci.ptScreenPos.y - originY - static_cast<int>(ii.yHotspot),
                   ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
 
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    }
 
    size_t frameBytes() const { return static_cast<size_t>(width) * height * 4; }
 
    ~ScreenCapture()
    {
        if (memDC && oldBmp) SelectObject(memDC, oldBmp);
        if (dib)      DeleteObject(dib);
        if (memDC)    DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }
};
 

struct FfmpegProcess {
    HANDLE hProcess    = nullptr;
    HANDLE hStdinWrite = nullptr;
 
    bool start(const std::wstring& outFile, int w, int h, int fps)
    {
        HANDLE readPipe = nullptr, writePipe = nullptr;
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        if (!CreatePipe(&readPipe, &writePipe, &sa, 1 << 20)) return false;
        SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, 0);
 
        std::wstringstream cmd;
        cmd << L"ffmpeg -hide_banner -loglevel error -y"
               L" -f rawvideo -pixel_format bgra"
               L" -video_size " << w << L"x" << h
            << L" -framerate " << fps
            << L" -i -"
               L" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\""
               L" -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p"
               L" -movflags +faststart"
               L" \"" << outFile << L"\"";
        std::wstring cmdline = cmd.str();
 
        STARTUPINFOW si{};
        si.cb          = sizeof(si);
        si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput   = readPipe;
        si.hStdOutput  = nullptr;
        si.hStdError   = nullptr;
 
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
 
        CloseHandle(readPipe);
        if (!ok) { CloseHandle(writePipe); return false; }
        CloseHandle(pi.hThread);
        hProcess    = pi.hProcess;
        hStdinWrite = writePipe;
        return true;
    }
 
    bool writeFrame(const void* data, size_t size)
    {
        const BYTE* p = static_cast<const BYTE*>(data);
        size_t total = 0;
        while (total < size) {
            DWORD written = 0;
            if (!WriteFile(hStdinWrite, p + total, static_cast<DWORD>(size - total), &written, nullptr))
                return false;
            total += written;
        }
        return true;
    }
 
    void finish()
    {
        if (hStdinWrite) { CloseHandle(hStdinWrite); hStdinWrite = nullptr; }
        if (hProcess) {
            WaitForSingleObject(hProcess, INFINITE);
            CloseHandle(hProcess);
            hProcess = nullptr;
        }
    }
};
 

static void recordThreadProc(int x, int y, int w, int h)
{
    ScreenCapture cap;
    long long framesWritten = 0;
    bool ok = true;
 
    if (!cap.init(x, y, w, h)) ok = false;
 
    FfmpegProcess enc;
    if (ok && !enc.start(OUT_FILE, cap.width, cap.height, FPS)) ok = false;
 
    if (ok) {
        timeBeginPeriod(1);
 
        using clock = std::chrono::steady_clock;
        const auto framePeriod = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / FPS));
 
        const auto started  = clock::now();
        auto       deadline = started;
 
        for (;;) {
            if (g_stopRequested.load()) break;
 
            const double elapsed = std::chrono::duration<double>(clock::now() - started).count();
            if (elapsed >= MAX_SECONDS) break;
 
            cap.grab();
 
            const long long targetFrames = static_cast<long long>(elapsed * FPS) + 1;
            while (framesWritten < targetFrames) {
                if (!enc.writeFrame(cap.pixels, cap.frameBytes())) { ok = false; break; }
                ++framesWritten;
                g_framesWritten.store(framesWritten);
            }
            if (!ok) break;
 
            deadline += framePeriod;
            const auto now = clock::now();
            if (deadline > now) std::this_thread::sleep_until(deadline);
            else while (deadline < now) deadline += framePeriod;
        }
 
        timeEndPeriod(1);
        enc.finish();
    }
 
    PostMessageW(g_hwnd, WM_RECORDING_DONE, ok ? 1 : 0, static_cast<LPARAM>(framesWritten));
}
 

enum class HitZone { None, Move, TL, T, TR, R, BR, B, BL, L };
 
static bool     g_dragging = false;
static HitZone  g_dragZone = HitZone::None;
static POINT    g_dragStartScreen{};
static RECT     g_dragStartRect{};
 
static HitZone hitTestOverlay(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
 
    const bool nearLeft   = p.x <= HANDLE_SIZE;
    const bool nearRight  = p.x >= rc.right - HANDLE_SIZE;
    const bool nearTop    = p.y <= HANDLE_SIZE;
    const bool nearBottom = p.y >= rc.bottom - HANDLE_SIZE;
    const bool inside     = p.x >= 0 && p.y >= 0 && p.x <= rc.right && p.y <= rc.bottom;
 
    if (nearLeft  && nearTop)    return HitZone::TL;
    if (nearRight && nearTop)    return HitZone::TR;
    if (nearLeft  && nearBottom) return HitZone::BL;
    if (nearRight && nearBottom) return HitZone::BR;
    if (nearTop)    return HitZone::T;
    if (nearBottom) return HitZone::B;
    if (nearLeft)   return HitZone::L;
    if (nearRight)  return HitZone::R;
    if (inside)     return HitZone::Move;
    return HitZone::None;
}
 
static HCURSOR cursorForZone(HitZone z)
{
    switch (z) {
    case HitZone::TL: case HitZone::BR: return LoadCursorW(nullptr, IDC_SIZENWSE);
    case HitZone::TR: case HitZone::BL: return LoadCursorW(nullptr, IDC_SIZENESW);
    case HitZone::L:  case HitZone::R:  return LoadCursorW(nullptr, IDC_SIZEWE);
    case HitZone::T:  case HitZone::B:  return LoadCursorW(nullptr, IDC_SIZENS);
    case HitZone::Move: return LoadCursorW(nullptr, IDC_SIZEALL);
    default: return LoadCursorW(nullptr, IDC_ARROW);
    }
}
 
static void notifyMainOfSelectionChange()
{
    PostMessageW(g_hwnd, WM_SELECTION_CHANGED, 0, 0);
}
 
static void paintOverlay(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
 
    HBRUSH keyBrush = CreateSolidBrush(OVERLAY_KEY);
    FillRect(hdc, &rc, keyBrush);
    DeleteObject(keyBrush);
 
    HBRUSH borderBrush = CreateSolidBrush(BORDER_COLOR);
    constexpr int B = 3; // border thickness
    RECT top{ rc.left, rc.top, rc.right, rc.top + B };
    RECT bottom{ rc.left, rc.bottom - B, rc.right, rc.bottom };
    RECT left{ rc.left, rc.top, rc.left + B, rc.bottom };
    RECT right{ rc.right - B, rc.top, rc.right, rc.bottom };
    FillRect(hdc, &top, borderBrush);
    FillRect(hdc, &bottom, borderBrush);
    FillRect(hdc, &left, borderBrush);
    FillRect(hdc, &right, borderBrush);
    DeleteObject(borderBrush);
 
    HBRUSH handleBrush = CreateSolidBrush(HANDLE_COLOR);
    auto drawHandle = [&](int cx, int cy) {
        RECT h{ cx - HANDLE_SIZE / 2, cy - HANDLE_SIZE / 2, cx + HANDLE_SIZE / 2, cy + HANDLE_SIZE / 2 };
        FillRect(hdc, &h, handleBrush);
    };
    drawHandle(rc.left, rc.top);
    drawHandle(rc.right, rc.top);
    drawHandle(rc.left, rc.bottom);
    drawHandle(rc.right, rc.bottom);
    drawHandle((rc.left + rc.right) / 2, rc.top);
    drawHandle((rc.left + rc.right) / 2, rc.bottom);
    drawHandle(rc.left, (rc.top + rc.bottom) / 2);
    drawHandle(rc.right, (rc.top + rc.bottom) / 2);
    DeleteObject(handleBrush);
 
    // Size label, top-left, on an opaque backing so it isn't colour-keyed away.
    wchar_t label[64];
    swprintf_s(label, L" %d x %d ", static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top));
    SIZE textSize;
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &textSize);
    RECT labelRect{ rc.left + B, rc.top + B, rc.left + B + textSize.cx, rc.top + B + textSize.cy };
    HBRUSH labelBg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &labelRect, labelBg);
    DeleteObject(labelBg);
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, labelRect.left, labelRect.top, label, static_cast<int>(wcslen(label)));
    SelectObject(hdc, oldFont);
 
    EndPaint(hwnd, &ps);
}
 
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        HitZone zone = hitTestOverlay(hwnd);
        if (zone != HitZone::None) {
            g_dragging = true;
            g_dragZone = zone;
            GetCursorPos(&g_dragStartScreen);
            GetWindowRect(hwnd, &g_dragStartRect);
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = false; g_dragZone = HitZone::None; ReleaseCapture(); }
        return 0;
 
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT cur; GetCursorPos(&cur);
            const int dx = cur.x - g_dragStartScreen.x;
            const int dy = cur.y - g_dragStartScreen.y;
            RECT r = g_dragStartRect;
 
            switch (g_dragZone) {
            case HitZone::Move:
                r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
                break;
            case HitZone::TL: r.left += dx; r.top += dy; break;
            case HitZone::TR: r.right += dx; r.top += dy; break;
            case HitZone::BL: r.left += dx; r.bottom += dy; break;
            case HitZone::BR: r.right += dx; r.bottom += dy; break;
            case HitZone::T:  r.top += dy; break;
            case HitZone::B:  r.bottom += dy; break;
            case HitZone::L:  r.left += dx; break;
            case HitZone::R:  r.right += dx; break;
            default: break;
            }
 
            // Enforce a minimum size by pinning whichever edge is moving.
            if (r.right - r.left < MIN_SEL_W) {
                if (g_dragZone == HitZone::L || g_dragZone == HitZone::TL || g_dragZone == HitZone::BL)
                    r.left = r.right - MIN_SEL_W;
                else
                    r.right = r.left + MIN_SEL_W;
            }
            if (r.bottom - r.top < MIN_SEL_H) {
                if (g_dragZone == HitZone::T || g_dragZone == HitZone::TL || g_dragZone == HitZone::TR)
                    r.top = r.bottom - MIN_SEL_H;
                else
                    r.bottom = r.top + MIN_SEL_H;
            }
 
            SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            notifyMainOfSelectionChange();
        }
        return 0;
 
    case WM_SETCURSOR:
        SetCursor(cursorForZone(g_dragging ? g_dragZone : hitTestOverlay(hwnd)));
        return TRUE;
 
    case WM_PAINT:
        paintOverlay(hwnd);
        return 0;
 
    case WM_ERASEBKGND:
        return 1;  // avoid a flash-paint with the default background before WM_PAINT
 
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
 
// Main window
static void setStatus(const std::wstring& text) { SetWindowTextW(g_lblStatus, text.c_str()); }
 
static void updateStatusFromSelection()
{
    if (g_recording.load()) return; 
    RECT r; GetWindowRect(g_overlay, &r);
    std::wstringstream ss;
    ss << L"Region: " << (r.right - r.left) << L" x " << (r.bottom - r.top)
       << L" at (" << r.left << L", " << r.top << L")";
    setStatus(ss.str());
}
 
static void startRecording()
{
    if (g_recording.load()) return;
    if (g_recordThread.joinable()) g_recordThread.join();
 
    RECT r; GetWindowRect(g_overlay, &r);
    const int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
 
    ShowWindow(g_overlay, SW_HIDE);   
 
    g_stopRequested.store(false);
    g_framesWritten.store(0);
    g_recording.store(true);
 
    EnableWindow(g_btnRecord, FALSE);
    EnableWindow(g_btnStop, TRUE);
    setStatus(L"Recording…");
    SetTimer(g_hwnd, TIMER_ID_UI, 250, nullptr);
 
    g_recordThread = std::thread(recordThreadProc, x, y, w, h);
}
 
static void stopRecording()
{
    if (!g_recording.load()) return;
    g_stopRequested.store(true);
    EnableWindow(g_btnStop, FALSE);
    setStatus(L"Stopping…");
}
 
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_btnRecord = CreateWindowW(L"BUTTON", L"Record", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     20, 20, 100, 32, hwnd, reinterpret_cast<HMENU>(IDC_RECORD),
                                     nullptr, nullptr);
        g_btnStop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   140, 20, 100, 32, hwnd, reinterpret_cast<HMENU>(IDC_STOP),
                                   nullptr, nullptr);
        g_lblStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                     20, 66, 280, 40, hwnd, nullptr, nullptr, nullptr);
        EnableWindow(g_btnStop, FALSE);
        return 0;
 
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_RECORD) startRecording();
        else if (LOWORD(wParam) == IDC_STOP) stopRecording();
        return 0;
 
    case WM_SELECTION_CHANGED:
        updateStatusFromSelection();
        return 0;
 
    case WM_TIMER:
        if (wParam == TIMER_ID_UI && g_recording.load()) {
            std::wstringstream ss;
            ss << L"Recording… " << g_framesWritten.load() << L" frames";
            setStatus(ss.str());
        }
        return 0;
 
    case WM_RECORDING_DONE: {
        KillTimer(hwnd, TIMER_ID_UI);
        g_recording.store(false);
        EnableWindow(g_btnRecord, TRUE);
        EnableWindow(g_btnStop, FALSE);
        ShowWindow(g_overlay, SW_SHOW);
 
        const bool      ok     = (wParam != 0);
        const long long frames = static_cast<long long>(lParam);
        std::wstringstream ss;
        if (ok) ss << L"Saved " << frames << L" frames to " << OUT_FILE;
        else    ss << L"Recording failed — is ffmpeg on PATH?";
        setStatus(ss.str());
        return 0;
    }
 
    case WM_DESTROY:
        if (g_recording.load()) g_stopRequested.store(true);
        if (g_recordThread.joinable()) g_recordThread.join();
        if (g_overlay) DestroyWindow(g_overlay);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
 
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    enableDpiAwareness();
 
    // Main window
    const wchar_t* mainClass = L"ScreenRecorderWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = mainClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
 
    g_hwnd = CreateWindowExW(0, mainClass, L"Screen Recorder",
                              (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME),
                              CW_USEDEFAULT, CW_USEDEFAULT, 340, 160,
                              nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
 
    // Selection overlay — default position: just below the main window.
    const wchar_t* overlayClass = L"ScreenRecorderSelectionOverlay";
    WNDCLASSW oc{};
    oc.lpfnWndProc   = OverlayWndProc;
    oc.hInstance     = hInstance;
    oc.lpszClassName = overlayClass;
    oc.hCursor       = nullptr;  // cursor is set manually via WM_SETCURSOR
    oc.hbrBackground = nullptr;
    RegisterClassW(&oc);
 
    RECT mainRect; GetWindowRect(g_hwnd, &mainRect);
    const int selX = mainRect.left;
    const int selY = mainRect.bottom + 20;
 
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 overlayClass, L"",
                                 WS_POPUP | WS_VISIBLE,
                                 selX, selY, DEFAULT_SEL_W, DEFAULT_SEL_H,
                                 nullptr, nullptr, hInstance, nullptr);
    SetLayeredWindowAttributes(g_overlay, OVERLAY_KEY, 0, LWA_COLORKEY);
    updateStatusFromSelection();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}