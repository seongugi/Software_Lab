#include "common.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>

Mapping mapping;
Modem modem;

inline void label(HDC dc, int y, const char* text) {
    RECT line{16, y, 336, y + 20};
    ExtTextOutA(dc, 16, y, ETO_OPAQUE, &line, text, lstrlenA(text), nullptr);
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

Nv12View video;
Frame sent{};
int frame = 0;
int currentBits = 4;

struct Color {
    std::uint8_t y, u, v;
};

Color sceneColor(int x, int y, int center) {
    if (y >= 19 && y < 23 && std::abs(x - center) < 4) return {208, 66, 144};
    if (y >= 17 && y < 28 && std::abs(x - center) < 7) return {115, 100, 200};
    if ((x - 51) * (x - 51) + (y - 8) * (y - 8) < 25) return {201, 48, 154};
    if (y >= 28) return {132, 96, 98};
    return {155, 166, 87};
}

void drawSource() {
    int center = 8 + frame % 48;
    std::uint8_t* yPlane = sent.data();
    std::uint8_t* uvPlane = yPlane + LumaBytes;

    for (int row = 0; row < H; ++row)
        for (int col = 0; col < W; ++col)
            yPlane[row * W + col] = sceneColor(col, row, center).y;

    for (int row = 0; row < H; row += 2) {
        for (int col = 0; col < W; col += 2) {
            Color color = sceneColor(col, row, center);
            int index = (row / 2) * W + col;
            uvPlane[index] = color.u;
            uvPlane[index + 1] = color.v;
        }
    }
}

void sendFrame(Link& link) {
    if (state(link) != Free)
        return;

    drawSource();
    link.original = sent;
    ++frame;

    currentBits = link.nextAdaptiveBits ? link.nextAdaptiveBits : 4;
    const int widths[Views] = {currentBits, 4, 2, 1};

    for (Wave& pilot : link.pilots)
        modem.encode(0, 1, pilot);

    for (int view = 0; view < Views; ++view) {
        int bits = widths[view];
        link.bits[view] = bits;
        int index = 0;
        for (std::uint8_t value : sent) {
            for (int shift = 8 - bits; shift >= 0; shift -= bits) {
                int symbol = (value >> shift) & ((1 << bits) - 1);
                modem.encode(symbol, bits, link.signal[view][index++]);
            }
        }
    }

    InterlockedExchange(&link.state, Sent);
}

void paint(HDC dc) {
    label(dc, 10, "Original NV12 frame");
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        if (!checkVideo(video.open(window, 16, 34), "Opening the NV12 renderer")) return -1;
        SetTimer(window, 1, 50, nullptr);
        return 0;

    case WM_TIMER:
        sendFrame(*mapping.link);
        if (frame && !checkVideo(video.show(sent), "Rendering the source frame"))
            DestroyWindow(window);
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
    wc.lpszClassName = "Sender";
    if (!RegisterClassA(&wc)) return 1;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT size{0, 0, 352, 255};
    AdjustWindowRect(&size, style, FALSE);

    HWND window = CreateWindowA("Sender", "Sender", style, 40, 60,
        size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}
