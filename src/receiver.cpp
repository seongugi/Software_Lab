#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

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

inline const char* mode(int bits) {
    return bits == 1 ? "BPSK" : bits == 2 ? "QPSK" : "16-QAM";
}

double c0[N], c1[N];

void makeBasis() {
    for (int k = 0; k < N; ++k) {
        double phase = 2 * std::acos(-1.0) * k / N;
        c0[k] = std::sqrt(2.0 / N) * std::cos(phase);
        c1[k] = std::sqrt(2.0 / N) * std::sin(phase);
    }
}

void point(int data, int width, double& i, double& q) {
    const int levels[] = {1, 3, -1, -3};
    if (width == 1) {
        i = 1 - 2 * data;
        q = 0;
    } else if (width == 2) {
        i = (1 - 2 * (data >> 1)) / std::sqrt(2.0);
        q = (1 - 2 * (data & 1)) / std::sqrt(2.0);
    } else {
        i = levels[data >> 2] / std::sqrt(10.0);
        q = levels[data & 3] / std::sqrt(10.0);
    }
}


void project(const Wave& r, double& i, double& q) {
    i = q = 0;
    for (int k = 0; k < N; ++k) {
        i += r[k] * c0[k];
        q += r[k] * c1[k];
    }
}

int decode(const Wave& r, int width) {
    double i, q;
    project(r, i, q);
    double best = 1e9;
    int decoded = 0;
    for (int data = 0; data < (1 << width); ++data) {
        double a, b;
        point(data, width, a, b);
        double distance = (i - a) * (i - a) + (q - b) * (q - b);
        if (distance < best) {
            best = distance;
            decoded = data;
        }
    }
    return decoded;
}

using Microsoft::WRL::ComPtr;

class Nv12View {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;

public:
    HRESULT open(HWND parent, int x, int y) {
        HWND window = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
            x, y, W * 5, H * 5, parent, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!window) return E_FAIL;
        DXGI_SWAP_CHAIN_DESC swap{};
        swap.BufferDesc.Width = W * 5;
        swap.BufferDesc.Height = H * 5;
        swap.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swap.SampleDesc.Count = 1;
        swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 1;
        swap.OutputWindow = window;
        swap.Windowed = TRUE;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &swap, swapChain.GetAddressOf(),
            device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (FAILED(hr)) return hr;

        ComPtr<ID3D11VideoDevice> videoDevice;
        hr = device.As(&videoDevice);
        if (FAILED(hr)) return hr;
        hr = context.As(&videoContext);
        if (FAILED(hr)) return hr;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {20, 1};
        content.OutputFrameRate = {20, 1};
        content.InputWidth = W;
        content.InputHeight = H;
        content.OutputWidth = W * 5;
        content.OutputHeight = H * 5;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        ComPtr<ID3D11VideoProcessorEnumerator> formats;
        hr = videoDevice->CreateVideoProcessorEnumerator(&content, formats.GetAddressOf());
        if (FAILED(hr)) return hr;
        UINT support = 0;
        hr = formats->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support);
        if (FAILED(hr)) return hr;
        if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) return E_NOTIMPL;
        hr = videoDevice->CreateVideoProcessor(formats.Get(), 0, processor.GetAddressOf());
        if (FAILED(hr)) return hr;

        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = W;
        texture.Height = H;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_NV12;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        hr = device->CreateTexture2D(&texture, nullptr, input.GetAddressOf());
        if (FAILED(hr)) return hr;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{};
        in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hr = videoDevice->CreateVideoProcessorInputView(input.Get(), formats.Get(), &in,
                                                        inputView.GetAddressOf());
        if (FAILED(hr)) return hr;

        ComPtr<ID3D11Texture2D> backBuffer;
        hr = swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr)) return hr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{};
        out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = videoDevice->CreateVideoProcessorOutputView(backBuffer.Get(), formats.Get(), &out,
                                                         outputView.GetAddressOf());
        if (FAILED(hr)) return hr;

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
        inputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inputColor);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        RECT source{0, 0, W, H};
        RECT target{0, 0, W * 5, H * 5};
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &source);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &target);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &target);
        return S_OK;
    }

    HRESULT show(const Frame& frame) {
        context->UpdateSubresource(input.Get(), 0, nullptr, frame.data(), W, 0);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView.Get();
        HRESULT hr = videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream);
        return FAILED(hr) ? hr : swapChain->Present(0, 0);
    }
};

bool checkVideo(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    char message[120];
    std::snprintf(message, sizeof message, "%s failed (0x%08lx).",
                  operation, static_cast<unsigned long>(hr));
    MessageBoxA(nullptr, message, "Direct3D 11", MB_OK | MB_ICONERROR);
    return false;
}

Nv12View video[Views];
Frame received[Views]{};
int currentBits[Views] = {4, 4, 2, 1};
int byteErrors[Views]{};
bool hasFrame = false;

void receiveFrame(Link& link) {
    if (state(link) != Noisy)
        return;

    double error = 0;
    for (const Wave& pilot : link.pilots) {
        double i, q;
        project(pilot, i, q);
        error += (i - 1) * (i - 1) + q * q;
    }

    // The only adaptive policy: pick the next frame's modulation
    // from the noise estimated using known pilot symbols.
    double sigma = std::sqrt(error / (2 * Pilots));
    const double qamNoiseLimit = 0.08;
    const double qpskNoiseLimit = 0.18;
    if (sigma < qamNoiseLimit)
        link.nextAdaptiveBits = 4;
    else if (sigma < qpskNoiseLimit)
        link.nextAdaptiveBits = 2;
    else
        link.nextAdaptiveBits = 1;

    for (int view = 0; view < Views; ++view) {
        int bits = link.bits[view];
        currentBits[view] = bits;
        int index = 0;
        for (auto& sample : received[view]) {
            int value = 0;
            for (int b = 0; b < 8; b += bits)
                value = (value << bits) | decode(link.signal[view][index++], bits);
            sample = static_cast<std::uint8_t>(value);
        }
    }

    // Count differing bytes across the complete NV12 frame.
    for (int view = 0; view < Views; ++view) {
        byteErrors[view] = 0;
        for (std::size_t i = 0; i < received[view].size(); ++i)
            byteErrors[view] += received[view][i] != link.original[i];
    }
    hasFrame = true;
    InterlockedExchange(&link.state, Free);
}

constexpr int PanelRowHeight = 254;

void paint(HDC dc) {
    const char* names[Views] = {"Adaptive", "Fixed", "Fixed", "Fixed"};
    for (int view = 0; view < Views; ++view) {
        int x = 16 + view % 2 * 336;
        int y = 10 + view / 2 * PanelRowHeight;
        char text[96];
        int bits = currentBits[view];
        if (hasFrame) {
            double errorRate = 100.0 * byteErrors[view] /
                               (LumaBytes + ChromaBytes);
            std::snprintf(text, sizeof text, "%s | %s | Error rate %.2f%%",
                          names[view], mode(bits), errorRate);
        } else {
            std::snprintf(text, sizeof text, "%s | %s | Waiting for frame",
                          names[view], mode(bits));
        }
        RECT line{x, y, x + 320, y + 22};
        ExtTextOutA(dc, x, y, ETO_OPAQUE, &line, text, lstrlenA(text), nullptr);

        RECT efficiencyLine{x, y + 22, x + 320, y + 44};
        if (hasFrame) {
            std::snprintf(text, sizeof text, "%d %s per symbol",
                          bits, bits == 1 ? "bit" : "bits");
        } else {
            text[0] = '\0';
        }
        ExtTextOutA(dc, x, y + 22, ETO_OPAQUE, &efficiencyLine, text, lstrlenA(text), nullptr);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        for (int view = 0; view < Views; ++view)
            if (!checkVideo(video[view].open(window, 16 + view % 2 * 336,
                    54 + view / 2 * PanelRowHeight), "Opening the NV12 renderer")) return -1;
        SetTimer(window, 1, 50, nullptr);
        return 0;
    case WM_TIMER:
        if (state(*mapping.link) != Noisy) return 0;
        receiveFrame(*mapping.link);
        for (int view = 0; view < Views; ++view) {
            if (!checkVideo(video[view].show(received[view]), "Rendering a received frame")) {
                DestroyWindow(window);
                return 0;
            }
        }
        InvalidateRect(window, nullptr, FALSE);
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
    makeBasis();

    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = "Receiver";
    if (!RegisterClassA(&wc)) return 1;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT size{0, 0, 688, 520};
    AdjustWindowRect(&size, style, FALSE);
    HWND window = CreateWindowA("Receiver", "Receiver", style, 425, 60,
        size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

