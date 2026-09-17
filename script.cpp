
//   g++ -O2 -std=c++17 -mwindows recorder_gui.cpp -o recorder_gui.exe -lgdi32 -luser32 -lwinmm
// Build (MSVC):
//   cl /O2 /EHsc /std:c++17 /SUBSYSTEM:WINDOWS recorder_gui.cpp user32.lib gdi32.lib winmm.lib

#include <windows.h>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
 
//edit here
constexpr int CAPTURE_X      = 0;
constexpr int CAPTURE_Y      = 0;
constexpr int CAPTURE_WIDTH  = 1920;
constexpr int CAPTURE_HEIGHT = 1080;
constexpr int FPS            = 60;
constexpr int MAX_SECONDS    = 60;        // safety auto-stop even if Stop isn't clicked
const wchar_t* OUT_FILE      = L"output.mp4";
 

static std::atomic<bool>      g_recording{false};
static std::atomic<bool>      g_stopRequested{false};
static std::atomic<long long> g_framesWritten{0};
static std::thread            g_recordThread;
 
static HWND g_hwnd = nullptr, g_btnRecord = nullptr, g_btnStop = nullptr, g_lblStatus = nullptr;
 
constexpr int  IDC_RECORD        = 101;
constexpr int  IDC_STOP          = 102;
constexpr UINT WM_RECORDING_DONE = WM_APP + 1;   // wParam: 1=ok/0=failed, lParam: frame count
constexpr UINT_PTR TIMER_ID_UI   = 1;
 
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
 
    bool init()
    {
        originX = CAPTURE_X;
        originY = CAPTURE_Y;
        width   = CAPTURE_WIDTH;
        height  = CAPTURE_HEIGHT;
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
    HANDLE hProcess   = nullptr;
    HANDLE hStdinWrite = nullptr;
 
    bool start(const std::wstring& outFile, int w, int h, int fps)
    {
        HANDLE readPipe = nullptr, writePipe = nullptr;
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };  // handles inheritable
        if (!CreatePipe(&readPipe, &writePipe, &sa, 1 << 20)) return false;
        // Only the read end should be inherited by ffmpeg; our write end must not be.
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
        std::wstring cmdline = cmd.str();  // CreateProcessW needs a mutable buffer
 
        STARTUPINFOW si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput  = readPipe;
        si.hStdOutput = nullptr;
        si.hStdError  = nullptr;
 
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                  /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW,
                                  nullptr, nullptr, &si, &pi);
 
        CloseHandle(readPipe);  // the child now owns its copy; parent's is done with it
        if (!ok) {
            CloseHandle(writePipe);
            return false;
        }
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
 
    // Closing the write handle sends ffmpeg EOF on stdin, which lets it flush
    // and finalise the MP4 (moov atom etc.) before the process exits.
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
 

static void recordThreadProc()
{
    ScreenCapture cap;
    long long framesWritten = 0;
    bool ok = true;
 
    if (!cap.init()) ok = false;
 
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
 
// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static void setStatus(const std::wstring& text) { SetWindowTextW(g_lblStatus, text.c_str()); }
 
static void startRecording()
{
    if (g_recording.load()) return;
 
    // A previous thread, if any, already posted WM_RECORDING_DONE and is
    // finishing up — join it before starting a new one so it's never
    // orphaned. This should return almost immediately.
    if (g_recordThread.joinable()) g_recordThread.join();
 
    g_stopRequested.store(false);
    g_framesWritten.store(0);
    g_recording.store(true);
 
    EnableWindow(g_btnRecord, FALSE);
    EnableWindow(g_btnStop, TRUE);
    setStatus(L"Recording…");
    SetTimer(g_hwnd, TIMER_ID_UI, 250, nullptr);
 
    g_recordThread = std::thread(recordThreadProc);
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
        g_lblStatus = CreateWindowW(L"STATIC", L"Idle", WS_CHILD | WS_VISIBLE,
                                     20, 66, 280, 24, hwnd, nullptr, nullptr, nullptr);
        EnableWindow(g_btnStop, FALSE);
        return 0;
 
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_RECORD) startRecording();
        else if (LOWORD(wParam) == IDC_STOP) stopRecording();
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
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
 
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    enableDpiAwareness();
 
    const wchar_t* className = L"ScreenRecorderWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = className;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
 
    g_hwnd = CreateWindowExW(0, className, L"Screen Recorder",
                              (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME),
                              CW_USEDEFAULT, CW_USEDEFAULT, 320, 150,
                              nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;
 
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
 
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}