#pragma once
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdint>
#include <cmath>

constexpr int W = 64, H = 40, N = 20;
constexpr int Pilots = 64;
constexpr int LumaBytes = W * H;
constexpr int ChromaBytes = LumaBytes / 2;
constexpr int FrameBits = 8 * (LumaBytes + ChromaBytes);
constexpr int Views = 4;

using Frame = std::array<std::uint8_t, LumaBytes + ChromaBytes>;
using Wave = std::array<float, N>;

enum { Free, Sent, Noisy };

struct Link {
    volatile LONG state;
    int nextAdaptiveBits;
    int bits[Views];
    Frame original;
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
        if (handle) {
            link = static_cast<Link*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Link)));
        }
    }

    ~Mapping() {
        if (link) UnmapViewOfFile(link);
        if (handle) CloseHandle(handle);
    }
};

inline LONG state(Link& link) {
    return InterlockedCompareExchange(&link.state, Free, Free);
}

inline const char* mode(int bits) {
    return bits == 1 ? "BPSK" : bits == 2 ? "QPSK" : "16-QAM";
}

struct Modem {
    double c0[N];
    double c1[N];

    Modem() {
        for (int k = 0; k < N; ++k) {
            double phase = 2.0 * std::acos(-1.0) * k / N;
            c0[k] = std::sqrt(2.0 / N) * std::cos(phase);
            c1[k] = std::sqrt(2.0 / N) * std::sin(phase);
        }
    }

    void point(int data, int width, double& i, double& q) const {
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

    void encode(int data, int width, Wave& wave) const {
        double i, q;
        point(data, width, i, q);
        for (int k = 0; k < N; ++k) {
            wave[k] = static_cast<float>(i * c0[k] + q * c1[k]);
        }
    }

    void project(const Wave& r, double& i, double& q) const {
        i = q = 0.0;
        for (int k = 0; k < N; ++k) {
            i += r[k] * c0[k];
            q += r[k] * c1[k];
        }
    }

    int decode(const Wave& r, int width) const {
        double i, q;
        project(r, i, q);
        double best = 1e9;
        int decoded = 0;
        for (int data = 0; data < (1 << width); ++data) {
            double a, b;
            point(data, width, a, b);
            double dist = (i - a) * (i - a) + (q - b) * (q - b);
            if (dist < best) {
                best = dist;
                decoded = data;
            }
        }
        return decoded;
    }
};
