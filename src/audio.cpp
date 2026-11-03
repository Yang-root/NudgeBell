#include "audio.h"
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <memory>

namespace vf {

static const float PI = 3.14159265358979f;
static const float TAU = 6.28318530717959f;

static float frand() { return (float)rand() / (float)RAND_MAX; }
static float frand(float a, float b) { return a + (b - a) * frand(); }
static int irand(int a, int b) { return a + rand() % (b - a + 1); }

// ---------- 基础波形 ----------
static inline float oscSine(float p) { return sinf(p * TAU); }
static inline float oscTri(float p) { p -= floorf(p); return 4.0f * fabsf(p - 0.5f) - 1.0f; }
static inline float oscSq(float p) { p -= floorf(p); return p < 0.5f ? 1.0f : -1.0f; }

// 指数衰减包络
static inline float envExp(float t, float atk, float dur) {
    if (t < 0.0f || t > dur) return 0.0f;
    if (t < atk) return atk <= 0.0f ? 1.0f : (t / atk);
    float k = (t - atk) / std::max(0.001f, dur - atk);
    return expf(-4.8f * k);
}

// 状态变量滤波器
struct SVF {
    float lp = 0, bp = 0;
    float process(float in, float f, float q) {
        float F = 2.0f * sinf(PI * std::min(f, SR * 0.45f) / (float)SR);
        float Q = 1.0f / std::max(0.5f, q);
        float hp = in - lp - Q * bp;
        bp += F * hp;
        lp += F * bp;
        return bp;
    }
    float low(float in, float f, float q) { process(in, f, q); return lp; }
    float high(float in, float f, float q) {
        float F = 2.0f * sinf(PI * std::min(f, SR * 0.45f) / (float)SR);
        float Q = 1.0f / std::max(0.5f, q);
        float hp = in - lp - Q * bp;
        bp += F * hp;
        lp += F * bp;
        return hp;
    }
};

struct Track {
    Buf& b;
    float amp;
    Track(Buf& buf) : b(buf), amp(1.0f) {}
    void mix(int i, float v) {
        if (i >= 0 && i < (int)b.size()) b[i] += v * amp;
    }
};

static void addTone(Buf& b, float freq, float t0, float dur, float peak,
                    int wave = 0, float glideTo = 0.0f, float atk = 0.006f) {
    int start = (int)(t0 * SR);
    int n = (int)(dur * SR);
    float phase = 0.0f;
    for (int i = 0; i < n; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= (int)b.size()) continue;
        float tt = (float)i / SR;
        float f = freq;
        if (glideTo > 0.0f) {
            float k = tt / std::max(0.001f, dur * 0.85f);
            if (k > 1.0f) k = 1.0f;
            f = freq * powf(glideTo / freq, k);
        }
        phase += f / SR;
        float v = 0.0f;
        switch (wave) {
            case 0: v = sinf(phase * TAU); break;
            case 1: v = oscTri(phase); break;
            case 2: v = oscSq(phase) * 0.7f; break;
            default: v = sinf(phase * TAU); break;
        }
        b[idx] += v * peak * envExp(tt, atk, dur);
    }
}

static void addNoise(Buf& b, float t0, float dur, float peak, float freq, float q,
                     int mode = 0, float atk = 0.004f) {
    int start = (int)(t0 * SR);
    int n = (int)(dur * SR);
    SVF f;
    for (int i = 0; i < n; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= (int)b.size()) continue;
        float tt = (float)i / SR;
        float x = frand() * 2.0f - 1.0f;
        float v = (mode == 0) ? f.process(x, freq, q) : f.high(x, freq, q);
        b[idx] += v * peak * envExp(tt, atk, dur);
    }
}

static Buf newBuf(float seconds) {
    return Buf((size_t)(seconds * SR) + 1, 0.0f);
}

static void normalize(Buf& b, float target = 0.85f) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, fabsf(v));
    if (m < 1e-6f) return;
    float k = target / m;
    for (float& v : b) v *= k;
}

// 五声音阶
static const float PENTA[8] = {523.25f, 587.33f, 659.25f, 783.99f, 880.0f, 1046.5f, 1174.66f, 1318.51f};

// ---------- 12 种音色 ----------
static Buf sndCrystal() {
    Buf b = newBuf(1.7f);
    const float bases[3] = {1046.5f, 1174.66f, 1318.51f};
    float base = bases[irand(0, 2)];
    const float mult[3] = {1.0f, 2.76f, 5.4f};
    const float dur[3] = {1.4f, 1.05f, 0.7f};
    const float amp[3] = {0.6f, 0.3f, 0.16f};
    for (int i = 0; i < 3; i++) addTone(b, base * mult[i], 0.0f, dur[i], amp[i]);
    addNoise(b, 0.0f, 0.06f, 0.12f, 6500.0f, 1.5f, 1);
    normalize(b);
    return b;
}

static Buf sndWoodblock() {
    Buf b = newBuf(0.35f);
    addNoise(b, 0.0f, 0.07f, 0.55f, frand(900.0f, 1300.0f), 7.0f, 0);
    addTone(b, frand(380.0f, 460.0f), 0.0f, 0.13f, 0.6f, 0, 240.0f, 0.002f);
    normalize(b);
    return b;
}

static Buf sndDrop() {
    Buf b = newBuf(0.7f);
    addTone(b, frand(1200.0f, 1700.0f), 0.0f, 0.18f, 0.65f, 0, frand(380.0f, 520.0f), 0.003f);
    addTone(b, 2200.0f, 0.004f, 0.08f, 0.24f);
    addNoise(b, 0.0f, 0.05f, 0.18f, 3000.0f, 3.0f, 0);
    normalize(b);
    return b;
}

static Buf sndChime() {
    Buf b = newBuf(2.4f);
    const float f[5] = {1568, 1760, 2093, 2349, 2637};
    int n = irand(3, 5);
    float t = 0.0f;
    for (int i = 0; i < n; i++) {
        addTone(b, f[irand(0, 4)], t, frand(1.0f, 1.9f), 0.3f, 0, 0.0f, 0.004f);
        t += frand(0.05f, 0.17f);
    }
    normalize(b, 0.8f);
    return b;
}

static Buf sndMusicBox() {
    Buf b = newBuf(1.4f);
    int st = irand(0, 4);
    for (int i = 0; i < 3; i++) addTone(b, PENTA[st + i], i * 0.14f, 0.8f, 0.45f, 1, 0.0f, 0.004f);
    normalize(b, 0.8f);
    return b;
}

static Buf sndBowl() {
    Buf b = newBuf(4.4f);
    const float bases[3] = {110.0f, 130.81f, 146.83f};
    float base = bases[irand(0, 2)];
    addTone(b, base, 0.0f, 4.0f, 0.55f, 0, 0.0f, 0.14f);
    addTone(b, base * 2.01f, 0.02f, 3.4f, 0.24f, 0, 0.0f, 0.16f);
    addTone(b, base * 3.02f, 0.05f, 2.4f, 0.12f, 0, 0.0f, 0.2f);
    normalize(b, 0.82f);
    return b;
}

static Buf sndPulse() {
    Buf b = newBuf(0.5f);
    addTone(b, 880.0f, 0.0f, 0.12f, 0.35f, 2, 0.0f, 0.003f);
    addTone(b, 880.0f, 0.0f, 0.24f, 0.35f, 0, 0.0f, 0.003f);
    addTone(b, 1320.0f, 0.13f, 0.12f, 0.35f, 2, 0.0f, 0.003f);
    addTone(b, 1320.0f, 0.13f, 0.26f, 0.35f, 0, 0.0f, 0.003f);
    normalize(b, 0.8f);
    return b;
}

static Buf sndBirds() {
    Buf b = newBuf(1.6f);
    int n = irand(2, 4);
    float t = 0.0f;
    for (int i = 0; i < n; i++) {
        float f0 = frand(2000.0f, 2800.0f);
        addTone(b, f0, t, 0.09f, 0.4f, 0, f0 * frand(1.2f, 1.55f), 0.01f);
        addTone(b, f0 * 1.02f, t + 0.09f, 0.08f, 0.3f, 0, f0 * 0.72f, 0.01f);
        t += frand(0.13f, 0.3f);
    }
    normalize(b, 0.8f);
    return b;
}

static Buf sndTriangle() {
    Buf b = newBuf(2.6f);
    const float f[3] = {2093, 2349, 2637};
    float base = f[irand(0, 2)];
    addTone(b, base, 0.0f, 2.4f, 0.45f, 0, 0.0f, 0.002f);
    addTone(b, base * 2.4f, 0.0f, 1.5f, 0.2f, 0, 0.0f, 0.002f);
    addNoise(b, 0.0f, 0.05f, 0.2f, 8000.0f, 1.0f, 1);
    normalize(b, 0.8f);
    return b;
}

static Buf sndStardust() {
    Buf b = newBuf(1.3f);
    const float f[6] = {1568, 1760, 2093, 2349, 2793, 3136};
    for (int i = 0; i < 9; i++) {
        addTone(b, f[irand(0, 5)], frand(0.0f, 0.45f), frand(0.25f, 0.7f), 0.22f, 0, 0.0f, 0.003f);
    }
    normalize(b, 0.78f);
    return b;
}

static Buf sndXylophone() {
    Buf b = newBuf(1.1f);
    int st = irand(0, 3);
    for (int i = 0; i < 4; i++) {
        addTone(b, PENTA[st + i] * 1.5f, i * 0.085f, 0.45f, 0.4f, 1, 0.0f, 0.003f);
    }
    normalize(b, 0.82f);
    return b;
}

static Buf sndGong() {
    Buf b = newBuf(4.0f);
    addTone(b, 98.0f, 0.0f, 3.6f, 0.6f, 0, 0.0f, 0.06f);
    addTone(b, 146.5f, 0.01f, 2.8f, 0.22f, 0, 0.0f, 0.09f);
    addTone(b, 392.0f, 0.03f, 1.8f, 0.12f, 0, 0.0f, 0.12f);
    addNoise(b, 0.0f, 1.2f, 0.2f, 900.0f, 2.0f, 0);
    normalize(b, 0.85f);
    return b;
}

Buf makeSound(int id) {
    switch (id) {
        case 0: return sndCrystal();
        case 1: return sndWoodblock();
        case 2: return sndDrop();
        case 3: return sndChime();
        case 4: return sndMusicBox();
        case 5: return sndBowl();
        case 6: return sndPulse();
        case 7: return sndBirds();
        case 8: return sndTriangle();
        case 9: return sndStardust();
        case 10: return sndXylophone();
        case 11: return sndGong();
        default: return sndCrystal();
    }
}

int soundCount() { return 12; }

static const wchar_t* NAMES[12] = {
    L"水晶铃", L"木鱼", L"水滴", L"风铃", L"八音盒", L"颂钵",
    L"电子脉冲", L"林间鸟鸣", L"三角铁", L"星尘", L"木琴上行", L"远钟"
};

static const wchar_t* TAGS[12] = {
    L"清透、短促，最不打扰",
    L"一声脆响，把思绪敲回当下",
    L"空谷一滴，安静到极点",
    L"错落的金属感，像有风吹过",
    L"温柔的三连音，带点怀旧",
    L"低频长鸣，帮你把呼吸放慢",
    L"两声叮，干净利落",
    L"几粒鸟叫，把你从屏幕里拉出来",
    L"一记长音，余韵很久",
    L"一串闪烁的碎音",
    L"四个音往上走，给人一点推力",
    L"沉到底的一声，适合深专注"
};

const wchar_t* soundName(int id) { return (id >= 0 && id < 12) ? NAMES[id] : L"水晶铃"; }
const wchar_t* soundTag(int id) { return (id >= 0 && id < 12) ? TAGS[id] : L""; }

// ---------- 环境音 ----------
Buf makeAmbient(int kind) {
    if (kind == 1) { // 雨声
        Buf b = newBuf(8.0f);
        SVF hp, lp;
        for (size_t i = 0; i < b.size(); i++) {
            float x = frand() * 2.0f - 1.0f;
            float v = lp.low(hp.high(x, 520.0f, 0.7f), 7200.0f, 0.7f);
            b[i] = v * 0.55f;
        }
        // 稀疏雨滴
        for (int i = 0; i < 60; i++) {
            addNoise(b, frand(0.0f, 7.6f), frand(0.05f, 0.13f), frand(0.05f, 0.14f),
                     frand(1600.0f, 4200.0f), 6.0f, 0);
        }
        normalize(b, 0.55f);
        return b;
    }
    if (kind == 2) { // 棕噪音
        Buf b = newBuf(8.0f);
        float last = 0.0f;
        SVF lp;
        for (size_t i = 0; i < b.size(); i++) {
            float w = frand() * 2.0f - 1.0f;
            last = (last + 0.02f * w) / 1.02f;
            b[i] = lp.low(last * 3.5f, 900.0f, 0.7f);
        }
        normalize(b, 0.55f);
        return b;
    }
    // 低频
    Buf b = newBuf(8.0f);
    const float f[3] = {110.0f, 164.81f, 220.0f};
    for (int k = 0; k < 3; k++) {
        float amp = 0.16f - k * 0.03f;
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / SR;
            float lfo = 0.6f + 0.4f * sinf(TAU * (0.05f + k * 0.03f) * t);
            b[i] += sinf(TAU * f[k] * t) * amp * lfo;
        }
    }
    // 首尾交叉淡化，避免循环爆音
    int fade = SR / 2;
    for (int i = 0; i < fade; i++) {
        float k = (float)i / fade;
        b[i] *= k;
        b[b.size() - 1 - i] *= k;
    }
    normalize(b, 0.5f);
    return b;
}

// ---------- waveOut 播放 ----------
static std::vector<short> toPcm(const Buf& b, float vol) {
    std::vector<short> out(b.size());
    for (size_t i = 0; i < b.size(); i++) {
        float v = b[i] * vol;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = (short)(v * 32000.0f);
    }
    return out;
}

static WAVEFORMATEX pcmFormat() {
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = SR;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = SR * 2;
    return wf;
}

void playOnce(const Buf& b, float volume) {
    if (b.empty() || volume <= 0.001f) return;
    auto pcm = std::make_shared<std::vector<short>>(toPcm(b, volume));
    std::thread([pcm]() {
        HWAVEOUT h = nullptr;
        WAVEFORMATEX wf = pcmFormat();
        if (waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
        WAVEHDR hdr = {};
        hdr.lpData = (LPSTR)pcm->data();
        hdr.dwBufferLength = (DWORD)(pcm->size() * 2);
        waveOutPrepareHeader(h, &hdr, sizeof(hdr));
        waveOutWrite(h, &hdr, sizeof(hdr));
        int guard = 0;
        while (!(hdr.dwFlags & WHDR_DONE) && guard++ < 2000) Sleep(10);
        waveOutUnprepareHeader(h, &hdr, sizeof(hdr));
        waveOutClose(h);
    }).detach();
}

// 环境音循环
static std::atomic<bool> g_ambStop{false};
static std::thread g_ambThread;
static std::mutex g_ambMutex;
static int g_ambKind = 0;
static float g_ambVol = 0.25f;

static void ambWorker(std::shared_ptr<std::vector<short>> pcm) {
    HWAVEOUT h = nullptr;
    WAVEFORMATEX wf = pcmFormat();
    if (waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    WAVEHDR hdr = {};
    hdr.lpData = (LPSTR)pcm->data();
    hdr.dwBufferLength = (DWORD)(pcm->size() * 2);
    waveOutPrepareHeader(h, &hdr, sizeof(hdr));

    while (!g_ambStop.load()) {
        hdr.dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(h, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) break;
        while (!(hdr.dwFlags & WHDR_DONE)) {
            if (g_ambStop.load()) break;
            Sleep(20);
        }
        if (g_ambStop.load()) break;
    }
    waveOutReset(h);
    waveOutUnprepareHeader(h, &hdr, sizeof(hdr));
    waveOutClose(h);
}

void stopAmbient() {
    std::lock_guard<std::mutex> lk(g_ambMutex);
    g_ambStop.store(true);
    if (g_ambThread.joinable()) g_ambThread.join();
    g_ambKind = 0;
}

void startAmbient(int kind, float volume) {
    stopAmbient();
    if (kind <= 0 || volume <= 0.001f) return;
    g_ambStop.store(false);
    g_ambVol = volume;
    g_ambKind = kind;
    auto pcm = std::make_shared<std::vector<short>>(toPcm(makeAmbient(kind), volume));
    std::lock_guard<std::mutex> lk(g_ambMutex);
    g_ambThread = std::thread(ambWorker, pcm);
}

void setAmbientVolume(float volume) {
    // 音量变化需要重建，简单处理：若正在播放则重启
    int k = g_ambKind;
    if (k > 0 && fabsf(volume - g_ambVol) > 0.01f) {
        startAmbient(k, volume);
    }
}

} // namespace vf
