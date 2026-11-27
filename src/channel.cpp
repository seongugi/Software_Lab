#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <random>

constexpr int W = 64, H = 40, N = 20;
constexpr int Pilots = 64;
constexpr int LumaBytes = W * H;
constexpr int ChromaBytes = LumaBytes / 2;
using Frame = std::array<std::uint8_t, LumaBytes + ChromaBytes>;
constexpr int FrameBits = 8 * (LumaBytes + ChromaBytes);
constexpr int Views = 4;
using Wave = std::array<float, N>;
enum { Free, Sent, Noisy };

// Shared-memory layout. The same definition is used in all three programs.
struct Link {
    volatile LONG state;
    int nextAdaptiveBits;
    int bits[Views];
    Frame original; // For error measurement only.
    Wave pilots[Pilots];
    Wave signal[Views][FrameBits];
};

class Mapping {
    HANDLE handle = nullptr;
public:
    Link* link = nullptr;
    Mapping() {
        handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, sizeof(Link), "AdaptiveVideoNv12Tuned");
        if (handle)
            link = static_cast<Link*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Link)));
    }
    ~Mapping() {
        if (link) UnmapViewOfFile(link);
        if (handle) CloseHandle(handle);
    }
};

inline LONG state(Link& link) {
    return InterlockedCompareExchange(&link.state, Free, Free);
}

Mapping mapping;

inline void label(HDC dc, int y, const char* text) {
    RECT line{16, y, 336, y + 20};
    ExtTextOutA(dc, 16, y, ETO_OPAQUE, &line, text, lstrlenA(text), nullptr);
}

HWND slider;
constexpr int MaxNoiseLevel = 1000;
int noiseLevel = 15;
std::mt19937 generator{42};
std::normal_distribution<double> gaussian{0, 1};

void addChannelNoise(Link& link) {
    if (state(link) != Sent)
        return;

    const double sigma = noiseLevel / 1000.0;

    for (Wave& pilot : link.pilots)
        for (float& sample : pilot)
            sample += static_cast<float>(sigma * gaussian(generator));

    // Replay the same noise sequence for each view. Longer transmissions
    // use more samples from that sequence.
    auto initialGenerator = generator;
    auto initialGaussian = gaussian;
    for (int view = 0; view < Views; ++view) {
        generator = initialGenerator;
        gaussian = initialGaussian;
        int symbols = FrameBits / link.bits[view];
        for (int k = 0; k < symbols; ++k) {
            for (float& sample : link.signal[view][k])
                sample += static_cast<float>(sigma * gaussian(generator));
        }
    }
    InterlockedExchange(&link.state, Noisy);
}

void controls(HWND window) {
    slider = CreateWindowA("SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_HORZ,
                           16, 58, 320, 22, window, nullptr, nullptr, nullptr);
    SetScrollRange(slider, SB_CTL, 0, MaxNoiseLevel, FALSE);
    SetScrollPos(slider, SB_CTL, noiseLevel, TRUE);
}

void scroll(WPARAM w) {
    switch (LOWORD(w)) {
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: noiseLevel = HIWORD(w); break;
    case SB_LINELEFT: --noiseLevel; break;
    case SB_LINERIGHT: ++noiseLevel; break;
    case SB_PAGELEFT: noiseLevel -= 20; break;
    case SB_PAGERIGHT: noiseLevel += 20; break;
    case SB_LEFT: noiseLevel = 0; break;
    case SB_RIGHT: noiseLevel = MaxNoiseLevel; break;
    }
    noiseLevel = std::clamp(noiseLevel, 0, MaxNoiseLevel);
    SetScrollPos(slider, SB_CTL, noiseLevel, TRUE);
}

void paint(HDC dc) {
    char text[100];
    std::snprintf(text, sizeof text, "Gaussian noise sigma = %.3f", noiseLevel / 1000.0);
    label(dc, 20, text);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        controls(window);
        SetTimer(window, 1, 50, nullptr);
        return 0;
    case WM_TIMER:
        addChannelNoise(*mapping.link);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_HSCROLL:
        scroll(w);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        paint(dc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

int main() {
    if (!mapping.link) {
        std::fprintf(stderr, "Could not open shared channel.\n");
        return 1;
    }
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = "Channel";
    if (!RegisterClassA(&wc)) return 1;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT size{0, 0, 352, 103};
    AdjustWindowRect(&size, style, FALSE);
    HWND window = CreateWindowA("Channel", "Channel", style, 40, 405,
        size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

