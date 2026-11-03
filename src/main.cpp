// NudgeBell Lite —— 纯 Win32 原生实现
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include "audio.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;
using namespace std;
using vf::Buf;

// ---------------- 常量 ----------------
static const int CW = 460;   // 逻辑宽
static const int CH = 680;   // 逻辑高
static const int TITLE_H = 52;

static const Color C_BG_TOP   (255, 26, 21, 51);
static const Color C_BG       (255, 10, 10, 16);
static const Color C_BG_BOT   (255, 7, 7, 12);
static const Color C_TEXT     (255, 236, 234, 245);
static const Color C_DIM      (150, 236, 234, 245);
static const Color C_DIMMER   (80, 236, 234, 245);
static const Color C_LINE     (24, 255, 255, 255);
static const Color C_PANEL    (12, 255, 255, 255);
static const Color C_PANEL2   (22, 255, 255, 255);
static const Color C_ACCENT   (255, 124, 92, 255);
static const Color C_ACCENT2  (255, 34, 211, 238);
static const Color C_WARN     (255, 255, 180, 87);

static float S = 1.0f;      // DPI 缩放
static HWND g_hwnd = nullptr;
static ULONG_PTR g_gdiToken = 0;
static HINSTANCE g_hInst = nullptr;
static int g_dpi = 96;
static bool g_testMode = false;   // 自检时不碰真实配置

// ---------------- 状态 ----------------
struct DayRec { wstring date; long long ms = 0; };

struct AppState {
    int mode = 0;              // 0 盲盒 1 定时
    int minGap = 3, maxGap = 6, fixedGap = 15;
    float volume = 0.7f;
    bool soundOn = true, cardOn = true, notifyOn = true, autoStart = false;
    bool topmost = false, bringFront = true;
    bool enabled[12];
    int ambient = 0;
    float ambVolume = 0.25f;
    bool paused = false;
    long long sessionStart = 0;
    wstring todayDate;
    long long focusMs = 0;
    int bells = 0;
    // 专注周期：专注 N 分后提醒休息 M 分
    bool cycleOn = true;
    int  focusCyc = 90;
    int  breakCyc = 20;
    int  breaks = 0;           // 今天休息次数
    vector<DayRec> hist;

    AppState() { for (int i = 0; i < 12; i++) enabled[i] = true; }
};

struct Runtime {
    bool running = false;
    long long nextBellAt = 0, lastBellAt = 0, remainingMs = 0;
    float flash = 0.0f;
    float angle = 0.0f;
    int page = 0;          // 0 主界面 1 设置
    int hover = -1, pressed = -1, activeSlider = -1;
    float scrollY = 0.0f, scrollMax = 0.0f;   // 设置页滚动
    float mainY = 0.0f, mainMax = 0.0f;        // 主界面整页滚动
    int viewMonth = 0;                         // 热力图显示月份偏移，0=本月
    int phase = 0;                        // 0 专注 1 休息
    long long phaseStart = 0, phaseEnd = 0;
    long long phaseLeftMs = 0;            // 暂停时记住本阶段还剩多久
    bool dirty = false;                   // 设置有改动还没点"保存"
    bool cardVisible = false;
    long long cardUntil = 0, cardStartAt = 0;
    wstring cardKind, cardText, cardSound;
    long long cardFocus = 0;
    int cardBells = 0;
    bool cardManual = false;
    long long toastUntil = 0;   // 必须是 long long：float 存 1.7e12 的分辨率只有 64 秒
    wstring toastText;
    bool hideTipShown = false;
    int trayLastSec = -1;
};

static AppState st;
static Runtime rt;

// ---------------- 控件 ID ----------------
enum {
    ID_NONE = -1,
    ID_BLIND = 100, ID_FIXED, ID_GEAR, ID_MIN, ID_CLOSE,
    ID_MAIN, ID_BELL, ID_MUTE,
    ID_SHEET_CLOSE, ID_R_MIN, ID_R_MAX, ID_R_FIXED, ID_R_VOL, ID_R_AMB,
    ID_SW_CYCLE, ID_R_CYC_FOCUS, ID_R_CYC_BREAK, ID_SAVE,
    ID_SW_CARD, ID_SW_NOTIFY, ID_SW_AUTO, ID_SW_TOPMOST, ID_SW_FRONT,
    ID_PREV_MONTH, ID_NEXT_MONTH,
    ID_STEP_M = 500,           // +滑杆ID → 减一档
    ID_STEP_P = 700,           // +滑杆ID → 加一档
    ID_SOUND_BASE = 200,       // +0..11
    ID_AMB_BASE = 300,         // +0..3
    ID_RESET = 400, ID_USERDATA
};

// ---------------- 工具 ----------------
static long long nowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

// ---------------- 专注周期（绘制代码会用到，放前面） ----------------
static const Color C_BREAK(255, 74, 222, 128);   // 休息态的绿

static long long phaseTotalMs() {
    return (long long)(rt.phase == 1 ? st.breakCyc : st.focusCyc) * 60000LL;
}

static float phaseProgress() {
    long long tot = phaseTotalMs();
    if (tot <= 0) return 0;
    float p = (float)((nowMs() - rt.phaseStart) / (double)tot);
    return min(1.0f, max(0.0f, p));
}

static wstring todayKey() {
    time_t t = time(nullptr);
    tm tmv;
    localtime_s(&tmv, &t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

static wstring fmtClock(long long ms) {
    long long s = ms / 1000;
    long long h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    wchar_t buf[32];
    if (h > 0) swprintf_s(buf, L"%lld:%02lld:%02lld", h, m, ss);
    else swprintf_s(buf, L"%02lld:%02lld", m, ss);
    return buf;
}

static wstring fmtMMSS(long long ms) {
    long long s = max(0LL, ms) / 1000;
    wchar_t buf[16];
    swprintf_s(buf, L"%02lld:%02lld", s / 60, s % 60);
    return buf;
}

static float frnd(float a, float b) { return a + (b - a) * ((float)rand() / RAND_MAX); }

static long long nextGapMs() {
    if (st.mode == 1) return (long long)st.fixedGap * 60000LL;
    int lo = min(st.minGap, st.maxGap), hi = max(st.minGap, st.maxGap);
    float r = (frnd(0, 1) + frnd(0, 1)) * 0.5f;
    return (long long)((lo + r * (hi - lo)) * 60000.0f);
}

// ---------------- 持久化 ----------------
static wstring g_stateFile;          // 缓存主数据文件路径，供"档被删了吗"检查用

static wstring statePath() {
    wchar_t* appdata = nullptr;
    size_t len = 0;
    _wdupenv_s(&appdata, &len, L"APPDATA");
    wstring dir = appdata ? appdata : L".";
    dir += L"\\NudgeBell";
    wstring ini = dir + (g_testMode ? L"\\state.test.ini" : L"\\state.ini");
    CreateDirectoryW(dir.c_str(), nullptr);

    // 从旧版本（VibeFocusLite）迁移数据，只做一次。
    // 用标记文件记住"已经迁过"——否则用户每次删档、每次重开，旧数据都会复活。
    wstring flag = dir + L"\\migrated";
    bool migrated = GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!g_testMode && !migrated) {
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES && appdata) {
            wstring oldIni = wstring(appdata) + L"\\VibeFocusLite\\state.ini";
            if (GetFileAttributesW(oldIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(oldIni.c_str(), ini.c_str(), FALSE);
                // 迁完就把旧文件改名收走，免得哪天用户把整个 NudgeBell 目录删了又复活一遍
                MoveFileW(oldIni.c_str(), (oldIni + L".migrated").c_str());
            }
        }
        HANDLE h = CreateFileW(flag.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    free(appdata);          // 必须在最后一次使用之后
    g_stateFile = ini;
    return ini;
}

static void loadState() {
    wstring p = statePath();
    auto gi = [&](const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"vibe", k, def, p.c_str()); };
    auto gs = [&](const wchar_t* k) {
        wchar_t buf[256] = {0};
        GetPrivateProfileStringW(L"vibe", k, L"", buf, 256, p.c_str());
        return wstring(buf);
    };

    st.mode = gi(L"mode", 0);
    st.minGap = gi(L"minGap", 3);
    st.maxGap = gi(L"maxGap", 6);
    st.fixedGap = gi(L"fixedGap", 15);
    st.volume = gi(L"volume", 70) / 100.0f;
    st.soundOn = gi(L"soundOn", 1) != 0;
    st.cardOn = gi(L"cardOn", 1) != 0;
    st.notifyOn = gi(L"notifyOn", 1) != 0;
    st.autoStart = gi(L"autoStart", 0) != 0;
    st.topmost = gi(L"topmost", 0) != 0;
    st.bringFront = gi(L"bringFront", 1) != 0;
    st.ambient = gi(L"ambient", 0);
    st.ambVolume = gi(L"ambVolume", 25) / 100.0f;
    st.paused = gi(L"paused", 0) != 0;
    st.cycleOn = gi(L"cycleOn", 1) != 0;
    st.focusCyc = gi(L"focusCyc", 90);
    st.breakCyc = gi(L"breakCyc", 20);
    st.breaks = gi(L"breaks", 0);
    st.sessionStart = _wtoi64(gs(L"sessionStart").c_str());
    st.todayDate = gs(L"todayDate");
    st.focusMs = _wtoi64(gs(L"focusMs").c_str());
    st.bells = gi(L"bells", 0);
    for (int i = 0; i < 12; i++) st.enabled[i] = gi((L"on" + to_wstring(i)).c_str(), 1) != 0;
    st.hist.clear();
    for (int i = 0; i < 400; i++) {          // 与热力图的 12 个月翻页对齐
        wstring v = gs((L"h" + to_wstring(i)).c_str());
        if (v.empty()) continue;
        size_t c = v.find(L',');
        if (c == wstring::npos) continue;
        DayRec d;
        d.date = v.substr(0, c);
        d.ms = _wtoi64(v.substr(c + 1).c_str());
        st.hist.push_back(d);
    }
}

static void saveState(bool withSettings = true) {
    wstring p = statePath();
    auto si = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"vibe", k, to_wstring(v).c_str(), p.c_str()); };
    if (withSettings) {
    si(L"mode", st.mode);
    si(L"minGap", st.minGap);
    si(L"maxGap", st.maxGap);
    si(L"fixedGap", st.fixedGap);
    si(L"volume", (int)(st.volume * 100));
    si(L"soundOn", st.soundOn ? 1 : 0);
    si(L"cardOn", st.cardOn ? 1 : 0);
    si(L"notifyOn", st.notifyOn ? 1 : 0);
    si(L"autoStart", st.autoStart ? 1 : 0);
    si(L"topmost", st.topmost ? 1 : 0);
    si(L"bringFront", st.bringFront ? 1 : 0);
    si(L"ambient", st.ambient);
    si(L"ambVolume", (int)(st.ambVolume * 100));
    si(L"paused", st.paused ? 1 : 0);
    si(L"cycleOn", st.cycleOn ? 1 : 0);
    si(L"focusCyc", st.focusCyc);
    si(L"breakCyc", st.breakCyc);
    for (int i = 0; i < 12; i++) si((L"on" + to_wstring(i)).c_str(), st.enabled[i] ? 1 : 0);
    }   // withSettings

    si(L"breaks", st.breaks);
    WritePrivateProfileStringW(L"vibe", L"sessionStart", to_wstring(st.sessionStart).c_str(), p.c_str());
    WritePrivateProfileStringW(L"vibe", L"todayDate", st.todayDate.c_str(), p.c_str());
    WritePrivateProfileStringW(L"vibe", L"focusMs", to_wstring(st.focusMs).c_str(), p.c_str());
    si(L"bells", st.bells);
    for (size_t i = 0; i < st.hist.size() && i < 400; i++) {
        wstring v = st.hist[i].date + L"," + to_wstring(st.hist[i].ms);
        WritePrivateProfileStringW(L"vibe", (L"h" + to_wstring(i)).c_str(), v.c_str(), p.c_str());
    }
}

static void ensureToday() {
    wstring k = todayKey();
    if (st.todayDate == k) return;
    if (!st.todayDate.empty() && st.focusMs > 0) {
        bool found = false;
        for (auto& d : st.hist) if (d.date == st.todayDate) { d.ms = st.focusMs; found = true; }
        if (!found) st.hist.push_back({ st.todayDate, st.focusMs });
        sort(st.hist.begin(), st.hist.end(), [](const DayRec& a, const DayRec& b) { return a.date < b.date; });
        if (st.hist.size() > 400) st.hist.erase(st.hist.begin(), st.hist.begin() + (st.hist.size() - 400));
    }
    st.todayDate = k;
    st.focusMs = 0;
    st.bells = 0;
}

static int computeStreak() {
    vector<wstring> days;
    for (auto& d : st.hist) if (d.ms >= 60000) days.push_back(d.date);
    if (st.focusMs >= 60000) days.push_back(st.todayDate);
    int streak = 0;
    time_t t = time(nullptr);
    tm tmv;
    localtime_s(&tmv, &t);
    for (int i = 0; i < 400; i++) {
        wchar_t buf[32];
        swprintf_s(buf, L"%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        wstring k = buf;
        bool has = false;
        for (auto& d : days) if (d == k) { has = true; break; }
        if (has) { streak++; tmv.tm_mday--; mktime(&tmv); }
        else if (i == 0) { tmv.tm_mday--; mktime(&tmv); }
        else break;
    }
    return streak;
}

// ---------------- 绘制辅助 ----------------


// GDI+ 的 REAL/INT 重载在 float 实参下会歧义，统一走 float 包装
static inline void fillEll(Graphics& g, Brush* b, float x, float y, float w, float h) {
    g.FillEllipse(b, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
}
static inline void drawEll(Graphics& g, Pen* p, float x, float y, float w, float h) {
    g.DrawEllipse(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
}
static inline void fillRect(Graphics& g, Brush* b, float x, float y, float w, float h) {
    g.FillRectangle(b, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
}
static inline void drawLn(Graphics& g, Pen* p, float x1, float y1, float x2, float y2) {
    g.DrawLine(p, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
}
static inline void drawArc(Graphics& g, Pen* p, float x, float y, float w, float h, float st, float sw) {
    g.DrawArc(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h, (REAL)st, (REAL)sw);
}

static void addRoundRect(GraphicsPath& path, RectF r, float rad, float = 0.0f) {
    if (rad < 0.1f) rad = 0.1f;
    float d = rad * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

static void fillRound(Graphics& g, RectF r, float rad, const Color& c) {
    GraphicsPath p;
    addRoundRect(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void strokeRound(Graphics& g, RectF r, float rad, const Color& c, float w) {
    GraphicsPath p;
    addRoundRect(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

static void textAt(Graphics& g, const wstring& s, RectF r, Font& f, const Color& c,
                   StringAlignment ha = StringAlignmentCenter,
                   StringAlignment va = StringAlignmentCenter) {
    StringFormat sf;
    sf.SetAlignment(ha);
    sf.SetLineAlignment(va);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, &f, r, &sf, &b);
}

static Font* fClock = nullptr;
static Font* fTitle = nullptr;
static Font* fBody = nullptr;
static Font* fSmall = nullptr;
static Font* fTiny = nullptr;
static Font* fBtn = nullptr;
static Font* fStat = nullptr;

static void initFonts() {
    if (fClock) return;
    fClock  = new Font(L"Segoe UI Light", 40.0f * 1.0f, FontStyleRegular, UnitPixel);
    fTitle  = new Font(L"Microsoft YaHei UI", 13.5f, FontStyleBold, UnitPixel);
    fBody   = new Font(L"Microsoft YaHei UI", 12.5f, FontStyleRegular, UnitPixel);
    fSmall  = new Font(L"Microsoft YaHei UI", 10.5f, FontStyleRegular, UnitPixel);
    fTiny   = new Font(L"Microsoft YaHei UI", 10.0f, FontStyleRegular, UnitPixel);
    fBtn    = new Font(L"Microsoft YaHei UI", 15.0f, FontStyleBold, UnitPixel);
    fStat   = new Font(L"Segoe UI", 17.0f, FontStyleRegular, UnitPixel);
}

// 按 DPI 缩放绘制
struct Scaler {
    Graphics* g;
    Scaler(Graphics* gg) : g(gg) { g->ScaleTransform(S, S); }
    ~Scaler() { g->ResetTransform(); }
};

// ---------------- 布局 ----------------
static RectF rTitleBar() { return RectF(0, 0, (float)CW, TITLE_H); }
static RectF rModeSeg()  { return RectF(228, 11, 104, 30); }
static RectF rGear()     { return RectF(340, 11, 30, 30); }
static RectF rMin()      { return RectF(374, 11, 30, 30); }
static RectF rClose()    { return RectF(408, 11, 30, 30); }
static RectF rRing()     { return RectF((CW - 288) / 2.0f, 114, 288, 288); }
static RectF rStatus()   { return RectF(0, 468, (float)CW, 28); }
static RectF rMainBtn()  { return RectF(24, 498, 300, 48); }
static RectF rSaveBtn()  { return RectF(CW - 136, CH - 46, 112, 32); }
static RectF rBell()     { return RectF(334, 498, 48, 48); }
static RectF rMute()     { return RectF(392, 498, 48, 48); }

static RectF statCard(int i, float baseY) {
    float w = (CW - 36 - 16) / 3.0f;
    return RectF(18 + i * (w + 8), baseY, w, 64);
}

// ---------------- 绘制：主界面 ----------------
static void drawBackground(Graphics& g) {
    SolidBrush sb(Color(255, 24, 20, 44));
    fillRect(g, &sb, 0, 0, (float)CW, (float)CH);

    // 顶部柔光：层数少、透明度差小，避免出现同心圆
    float k = (st.sessionStart != 0 && rt.running) ? 1.0f : 0.5f;
    for (int i = 0; i < 7; i++) {
        float rad = 170.0f + i * 48.0f;
        int a = (int)((7.0f - i * 0.8f) * k);
        if (a <= 0) continue;
        SolidBrush b(Color(a, 124, 92, 255));
        fillEll(g, &b, CW / 2.0f - rad, -80 - rad * 0.28f, rad * 2, rad * 1.1f);
    }
}

// 右侧滚动条指示（整页滚动时才有）
static void drawStreamBar(Graphics& g) {
    if (rt.mainMax < 4) return;
    float trackTop = TITLE_H + 12;
    float trackH = CH - TITLE_H - 24;
    float visible = (float)(CH - TITLE_H);
    float thumbH = max(34.0f, trackH * visible / (visible + rt.mainMax));
    float p = rt.mainY / rt.mainMax;
    float y = trackTop + p * (trackH - thumbH);
    fillRound(g, RectF(CW - 6, y, 3.5f, thumbH), 1.75f, Color(52, 255, 255, 255));
}

static void drawTopBar(Graphics& g, float t) {
    // logo
    fillRound(g, RectF(16, 18, 9, 9), 2.5f, C_ACCENT);
    SolidBrush glow(Color(60, 124, 92, 255));
    fillEll(g, &glow, 12, 14, 17, 17);
    fillRound(g, RectF(16, 18, 9, 9), 2.5f, C_ACCENT);

    textAt(g, L"NudgeBell", RectF(32, 12, 120, 20), *fTitle, C_TEXT, StringAlignmentNear);

    // 模式切换
    RectF seg = rModeSeg();
    fillRound(g, seg, 9, Color(13, 255, 255, 255));
    strokeRound(g, seg, 9, C_LINE, 1.0f);
    float half = seg.Width / 2.0f;
    for (int i = 0; i < 2; i++) {
        bool on = (st.mode == i);
        RectF rr(seg.X + 2 + i * (half - 2), seg.Y + 2, half - 2, seg.Height - 4);
        if (on) {
            fillRound(g, rr, 7, Color(56, 124, 92, 255));
            strokeRound(g, rr, 7, Color(90, 124, 92, 255), 1.0f);
        }
        textAt(g, i == 0 ? L"盲盒" : L"定时", rr, *fSmall, on ? Color(255, 217, 210, 255) : C_DIM);
    }

    // 齿轮
    RectF gr = rGear();
    fillRound(g, gr, 9, rt.hover == ID_GEAR ? C_PANEL2 : C_PANEL);
    strokeRound(g, gr, 9, C_LINE, 1.0f);
    {
        GraphicsState gs = g.Save();
        g.TranslateTransform(gr.X + 15, gr.Y + 15);
        Pen pen(C_DIM, 1.5f);
        drawEll(g, &pen, -4.0f, -4.0f, 8.0f, 8.0f);
        drawEll(g, &pen, -1.6f, -1.6f, 3.2f, 3.2f);
        for (int i = 0; i < 6; i++) {
            float a = i * 3.14159f / 3.0f;
            drawLn(g, &pen, cosf(a) * 4.0f, sinf(a) * 4.0f, cosf(a) * 6.4f, sinf(a) * 6.4f);
        }
        g.Restore(gs);
    }

    // 最小化
    RectF mn = rMin();
    fillRound(g, mn, 9, rt.hover == ID_MIN ? C_PANEL2 : C_PANEL);
    strokeRound(g, mn, 9, C_LINE, 1.0f);
    {
        Pen pen(C_DIM, 1.8f);
        drawLn(g, &pen, mn.X + 9, mn.Y + 15, mn.X + 21, mn.Y + 15);
    }

    // 关闭
    RectF cl = rClose();
    fillRound(g, cl, 9, rt.hover == ID_CLOSE ? C_PANEL2 : C_PANEL);
    strokeRound(g, cl, 9, C_LINE, 1.0f);
    {
        Pen pen(C_DIM, 1.6f);
        drawLn(g, &pen, cl.X + 10, cl.Y + 10, cl.X + 20, cl.Y + 20);
        drawLn(g, &pen, cl.X + 20, cl.Y + 10, cl.X + 10, cl.Y + 20);
    }
}

static void drawRing(Graphics& g, float t) {
    RectF r = rRing();
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    float R = 116;

    bool running = rt.running;
    bool onBreak = st.cycleOn && rt.phase == 1 && st.sessionStart != 0;
    float ratio = 0;
    if (rt.running) {
        if (onBreak) {
            ratio = phaseProgress();
        } else {
            long long span = rt.nextBellAt - rt.lastBellAt;
            if (span > 0) ratio = (float)((nowMs() - rt.lastBellAt) / (double)span);
            ratio = min(1.0f, max(0.0f, ratio));
        }
    }
    bool urgent = running && !onBreak &&
                  (st.mode == 1 ? (rt.nextBellAt - nowMs() < 60000) : ratio > 0.9f);

    rt.angle += running ? 0.0016f * (urgent ? 2.6f : 1.0f) : 0.0004f;
    if (rt.angle > 1.0f) rt.angle -= 1.0f;

    // 中心光晕
    float breathe = 0.5f + 0.5f * sinf(t * 0.9f);
    float glowR = 74 + breathe * 8 + rt.flash * 26;
    for (int i = 0; i < 9; i++) {
        float rr = glowR * (0.55f + i * 0.13f);
        float baseA = 5.0f - i * 0.42f;
        int a = (int)((running ? baseA * (0.8f + breathe * 0.45f) : baseA * 0.32f) + rt.flash * 14);
        if (a <= 0) continue;
        a = min(60, a);
        SolidBrush b(onBreak ? Color(a, 74, 222, 128) : Color(a, 124, 92, 255));
        fillEll(g, &b, cx - rr, cy - rr, rr * 2, rr * 2);
    }

    // 点阵
    const int N = 54;
    for (int i = 0; i < N; i++) {
        float base = (float)i / N * 6.2831853f;
        float ang = base + rt.angle * 6.2831853f;
        float ph = t * (running ? (urgent ? 3.4f : 1.5f) : 0.5f) - i * 0.22f;
        float wave = 0.5f + 0.5f * sinf(ph);

        float alpha, size;
        if (st.sessionStart == 0) { alpha = 0.15f; size = 1.9f; }
        else if (!running) { alpha = 0.14f + wave * 0.06f; size = 2.0f; }
        else { alpha = 0.16f + wave * (urgent ? 0.55f : 0.34f); size = 1.9f + wave * (urgent ? 2.1f : 1.2f); }
        alpha = min(1.0f, alpha + rt.flash * 0.7f);
        size += rt.flash * 1.6f;

        float x = cx + cosf(ang) * R;
        float y = cy + sinf(ang) * R;

        float k = base / 6.2831853f + t * 0.02f;
        k = k - floorf(k);
        int cr = (int)((onBreak ? 52 : 124) + ((onBreak ? 163 : 34) - (onBreak ? 52 : 124)) * k);
        int cg = (int)((onBreak ? 211 : 92) + ((onBreak ? 230 : 211) - (onBreak ? 211 : 92)) * k);
        int cb = (int)((onBreak ? 153 : 255) + ((onBreak ? 53 : 238) - (onBreak ? 153 : 255)) * k);

        if (running && size > 2.2f) {
            int ga = (int)(alpha * 22);
            SolidBrush halo(Color(ga, cr, cg, cb));
            float hr = size * 3.4f;
            fillEll(g, &halo, x - hr, y - hr, hr * 2, hr * 2);
        }
        SolidBrush b(Color((int)(alpha * 255), cr, cg, cb));
        fillEll(g, &b, x - size, y - size, size * 2, size * 2);
    }

    // 内圈
    Pen pen(Color(running ? 16 : 10, 255, 255, 255), 1.0f);
    drawEll(g, &pen, cx - 104, cy - 104, 208, 208);

    // 扫描弧
    if (running) {
        float start = fmodf(t * 0.7f, 6.2831853f) * 180.0f / 3.14159f;
        Pen arc(onBreak ? Color(90, 74, 222, 128) : Color(90, 124, 92, 255), 1.4f);
        drawArc(g, &arc, cx - (R - 12), cy - (R - 12), (R - 12) * 2, (R - 12) * 2, start, 52);
    }

    // 本轮进度：专注段从 0 走到满，休息段反向走回来
    if (st.cycleOn && st.sessionStart != 0) {
        float pr = phaseProgress();
        Pen track(Color(20, 255, 255, 255), 2.0f);
        drawEll(g, &track, cx - (R + 11), cy - (R + 11), (R + 11) * 2, (R + 11) * 2);
        if (pr > 0.003f) {
            Pen pg(onBreak ? Color(210, 74, 222, 128) : Color(165, 124, 92, 255), 2.6f);
            drawArc(g, &pg, cx - (R + 11), cy - (R + 11), (R + 11) * 2, (R + 11) * 2, -90, pr * 360.0f);
        }
    }

    if (rt.flash > 0.002f) rt.flash *= 0.94f; else rt.flash = 0;

    // 中间文字：休息段显示倒计时
    wstring clock = onBreak ? fmtMMSS(max(0LL, rt.phaseEnd - nowMs())) : fmtClock(st.focusMs);
    Font* cf = fClock;
    if (clock.size() > 5) {
        static Font narrow(L"Segoe UI Light", 34.0f, FontStyleRegular, UnitPixel);
        cf = &narrow;
    }
    textAt(g, clock, RectF(cx - 110, cy - 32, 220, 46), *cf, Color(255, 255, 255, 255));
    textAt(g, onBreak ? L"休 息 中" : L"已 专 注", RectF(cx - 110, cy + 14, 220, 18), *fTiny,
           onBreak ? Color(190, 74, 222, 128) : C_DIMMER);
}

static void drawStatus(Graphics& g) {
    RectF r = rStatus();
    wstring s;
    Color dotC = C_ACCENT2;
    bool soon = false;

    if (st.sessionStart == 0) { s = L"还没开始 · 一次只做一件事"; dotC = C_DIMMER; }
    else if (!rt.running) { s = L"已暂停 · 随时可以回来"; dotC = C_DIMMER; }
    else if (st.cycleOn && rt.phase == 1) {
        s = L"该起来了 · 休息还剩 " + fmtMMSS(max(0LL, rt.phaseEnd - nowMs()));
        dotC = C_BREAK;
    }
    else if (st.cycleOn && rt.phaseEnd > nowMs() && rt.phaseEnd - nowMs() < 180000) {
        long long left = rt.phaseEnd - nowMs();
        s = L"本轮快满了 · 还有 " + to_wstring(left / 60000 + 1) + L" 分";
        dotC = C_BREAK;
        soon = true;
    }
    else if (st.mode == 1) {
        long long left = rt.nextBellAt - nowMs();
        s = L"下一个提示音 · " + fmtMMSS(left);
        soon = left < 60000;
    } else {
        long long span = rt.nextBellAt - rt.lastBellAt;
        float ratio = span > 0 ? (float)((nowMs() - rt.lastBellAt) / (double)span) : 0;
        if (ratio < 0.3f) s = L"刚安静过 · 沉进去待一会儿";
        else if (ratio < 0.7f) s = L"正在中间 · 别抬头";
        else if (ratio < 0.9f) s = L"一天里最好的一段";
        else { s = L"就快了 · 保持住"; soon = true; }
    }
    if (soon) dotC = C_WARN;

    // 呼吸点：先量出文字宽度，再把点放到文字左边，避免压在字上
    RectF tb;
    StringFormat mf;
    mf.SetAlignment(StringAlignmentNear);
    mf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.MeasureString(s.c_str(), -1, fBody, PointF(0, 0), &mf, &tb);
    float tw = tb.Width;
    float tx = r.X + (r.Width - tw) / 2;
    float dcx = tx - 12;
    float dcy = r.Y + r.Height / 2;

    float t = (float)GetTickCount64() / 1000.0f;
    float ph = soon ? (0.5f + 0.5f * sinf(t * 7.0f)) : (0.5f + 0.5f * sinf(t * 2.6f));
    int a = (int)((0.35f + 0.65f * ph) * 255);

    // 柔和光晕（原来是个 20px 实心圆，太抢眼）
    SolidBrush glow(Color(a / 7, dotC.GetR(), dotC.GetG(), dotC.GetB()));
    fillEll(g, &glow, dcx - 6.5f, dcy - 6.5f, 13, 13);
    SolidBrush dot(Color(a, dotC.GetR(), dotC.GetG(), dotC.GetB()));
    float dr = 2.6f + ph * 0.6f;
    fillEll(g, &dot, dcx - dr, dcy - dr, dr * 2, dr * 2);

    textAt(g, s, RectF(r.X + 10, r.Y, r.Width, r.Height), *fBody, Color(158, 236, 234, 245));
}

static void drawButtons(Graphics& g) {
    RectF mb = rMainBtn();
    bool running = rt.running;
    bool started = st.sessionStart != 0;

    if (!started || !running) {
        LinearGradientBrush lgb(PointF(mb.X, mb.Y), PointF(mb.X + mb.Width, mb.Y + mb.Height),
                                C_ACCENT, C_ACCENT2);
        lgb.SetWrapMode(WrapModeClamp);
        GraphicsPath p;
        addRoundRect(p, mb, 14, 0.5f);
        g.FillPath(&lgb, &p);
        // 顶部高光
        LinearGradientBrush hi(PointF(0, mb.Y), PointF(0, mb.Y + mb.Height),
                               Color(52, 255, 255, 255), Color(0, 255, 255, 255));
        hi.SetWrapMode(WrapModeClamp);
        g.FillPath(&hi, &p);
    } else {
        fillRound(g, mb, 14, Color(18, 255, 255, 255));
        strokeRound(g, mb, 14, Color(34, 255, 255, 255), 1.0f);
    }

    bool onBreak = st.cycleOn && rt.phase == 1 && started;
    wstring label;
    if (!started)            label = L"开始专注";
    else if (running && onBreak)  label = L"跳过休息";
    else if (running)        label = L"暂停一下";
    else if (onBreak)        label = L"继续休息";
    else                     label = L"继续专注";
    textAt(g, label, mb, *fBtn, !started || !running ? Color(255, 255, 255, 255) : C_TEXT);

    // 铃铛
    RectF bl = rBell();
    fillRound(g, bl, 14, rt.hover == ID_BELL ? C_PANEL2 : C_PANEL);
    strokeRound(g, bl, 14, C_LINE, 1.0f);
    {
        GraphicsState gs = g.Save();
        g.TranslateTransform(bl.X + 24, bl.Y + 24);
        Pen pen(C_DIM, 1.7f);
        GraphicsPath path;
        path.AddArc(-7.0f, -8.0f, 14.0f, 14.0f, 180, 180);
        path.AddLine(7.0f, -1.0f, 9.0f, 6.0f);
        path.AddLine(9.0f, 6.0f, -9.0f, 6.0f);
        path.AddLine(-9.0f, 6.0f, -7.0f, -1.0f);
        g.DrawPath(&pen, &path);
        drawArc(g, &pen, -2.5f, 6.0f, 5.0f, 5.0f, 0, 180);
        g.Restore(gs);
    }

    // 静音
    RectF mu = rMute();
    fillRound(g, mu, 14, rt.hover == ID_MUTE ? C_PANEL2 : C_PANEL);
    strokeRound(g, mu, 14, C_LINE, 1.0f);
    {
        GraphicsState gs = g.Save();
        g.TranslateTransform(mu.X + 24, mu.Y + 24);
        Pen pen(st.soundOn ? C_DIM : Color(90, 236, 234, 245), 1.7f);
        SolidBrush br(st.soundOn ? C_DIM : Color(90, 236, 234, 245));
        GraphicsPath p;
        p.AddLine(-8.0f, -3.0f, -3.0f, -3.0f);
        p.AddLine(-3.0f, -3.0f, 2.0f, -8.0f);
        p.AddLine(2.0f, -8.0f, 2.0f, 8.0f);
        p.AddLine(2.0f, 8.0f, -3.0f, 3.0f);
        p.AddLine(-3.0f, 3.0f, -8.0f, 3.0f);
        p.CloseFigure();
        g.DrawPath(&pen, &p);
        if (st.soundOn) {
            drawArc(g, &pen, 2.0f, -6.0f, 10.0f, 12.0f, -55, 110);
            drawArc(g, &pen, 2.0f, -10.0f, 17.0f, 20.0f, -50, 100);
        } else {
            drawLn(g, &pen, 5.0f, -6.0f, 13.0f, 6.0f);
            drawLn(g, &pen, 13.0f, -6.0f, 5.0f, 6.0f);
        }
        g.Restore(gs);
    }
}

// 专注时长映射到热力色阶
static Color heatColor(long long ms) {
    // 五档按亮度单调递增排布，相邻档差距要大，否则一眼看不出深浅
    if (ms <= 0)              return Color(255, 38, 36, 62);      // 没学
    if (ms < 15 * 60000LL)    return Color(255, 72, 58, 150);     // 15 分
    if (ms < 30 * 60000LL)    return Color(255, 124, 92, 220);    // 30 分
    if (ms < 60 * 60000LL)    return Color(255, 178, 140, 255);   // 1 小时
    return Color(255, 34, 211, 238);                              // 2 小时以上
}

// 热力图网格尺寸（renderAll 用它算滚动上限，drawStats 用它排版）
static const float HEAT_CELL = 46.0f;
static const float HEAT_GAP  = 8.0f;
static const float HEAT_STEP = HEAT_CELL + HEAT_GAP;
static const float HEAT_TOP  = 560.0f + 64.0f + 12.0f;   // 卡片顶部 y

// 当前查看月份在日历上占几行（4~6），月份总行数决定卡片高度
static int heatRows() {
    time_t nowT = time(nullptr);
    tm base;
    localtime_s(&base, &nowT);
    int totalMon = (base.tm_year + 1900) * 12 + base.tm_mon + rt.viewMonth;
    int year = totalMon / 12, mon0 = totalMon % 12;
    if (mon0 < 0) { mon0 += 12; year -= 1; }
    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dn = DIM[mon0];
    if (mon0 == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) dn = 29;
    tm first = base;
    first.tm_year = year - 1900;
    first.tm_mon = mon0;
    first.tm_mday = 1;
    mktime(&first);
    int startCol = (first.tm_wday + 6) % 7;   // 周一为第一列
    return (startCol + dn + 6) / 7;
}

static float heatCardH() {
    return heatRows() * HEAT_STEP - HEAT_GAP + 202.0f;
}

static void drawStats(Graphics& g) {

    const wchar_t* keys[3] = { L"今日专注", L"今日唤醒", L"连续记录" };
    wstring vals[3] = {
        to_wstring(st.focusMs / 60000),
        to_wstring(st.bells),
        to_wstring(computeStreak())
    };
    const wchar_t* units[3] = { L"分", L"次", L"天" };

    float top = 560;

    // ---- 三个统计卡 ----
    for (int i = 0; i < 3; i++) {
        RectF c = statCard(i, top);
        fillRound(g, c, 12, C_PANEL);
        strokeRound(g, c, 12, C_LINE, 1.0f);

        textAt(g, vals[i], RectF(c.X + 12, c.Y + 7, 60, 22), *fStat, C_TEXT, StringAlignmentNear);
        float vw = 0;
        {
            RectF b;
            StringFormat sf;
            sf.SetAlignment(StringAlignmentNear);
            g.MeasureString(vals[i].c_str(), -1, fStat, PointF(0, 0), &sf, &b);
            vw = b.Width;
        }
        textAt(g, units[i], RectF(c.X + 14 + vw, c.Y + 10, 24, 16), *fTiny, C_DIM, StringAlignmentNear);
        textAt(g, keys[i], RectF(c.X + 12, c.Y + 36, c.Width - 24, 16), *fTiny, C_DIMMER, StringAlignmentNear);
    }

    // ---- 月历热力图 ----
    float hy = HEAT_TOP;
    RectF hc(18, hy, CW - 36, heatCardH());
    fillRound(g, hc, 16, C_PANEL);
    strokeRound(g, hc, 16, C_LINE, 1.0f);

    time_t nowT = time(nullptr);
    tm base;
    localtime_s(&base, &nowT);

    // 按偏移量换算出要显示的月份
    int totalMon = (base.tm_year + 1900) * 12 + base.tm_mon + rt.viewMonth;
    int year = totalMon / 12;
    int mon0 = totalMon % 12;
    if (mon0 < 0) { mon0 += 12; year -= 1; }
    bool isThisMonth = (rt.viewMonth == 0);

    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int daysInMonth = DIM[mon0];
    if (mon0 == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) daysInMonth = 29;

    tm first = base;
    first.tm_year = year - 1900;
    first.tm_mon = mon0;
    first.tm_mday = 1;
    mktime(&first);
    int startCol = (first.tm_wday + 6) % 7;   // 周一为第一列

    auto dayMs = [&](int d) -> long long {
        wchar_t buf[32];
        swprintf_s(buf, L"%04d-%02d-%02d", year, mon0 + 1, d);
        wstring k = buf;
        if (k == st.todayDate) return st.focusMs;
        for (auto& h : st.hist) if (h.date == k) return h.ms;
        return 0;
    };

    // 标题
    wchar_t title[64];
    if (isThisMonth) swprintf_s(title, L"%d 年 %d 月 · 本月", year, mon0 + 1);
    else swprintf_s(title, L"%d 年 %d 月", year, mon0 + 1);
    textAt(g, title, RectF(hc.X, hc.Y + 16, hc.Width, 22), *fBody, C_TEXT);

    // 左右翻月按钮
    const float NAV = 34.0f;
    RectF prev(hc.X + 14, hc.Y + 12, NAV, 28);
    RectF next(hc.GetRight() - 14 - NAV, hc.Y + 12, NAV, 28);
    auto navBtn = [&](RectF r, bool right) {
        bool on = right ? (rt.viewMonth < 0) : (rt.viewMonth > -11);
        int id = right ? ID_NEXT_MONTH : ID_PREV_MONTH;
        fillRound(g, r, 9, rt.hover == id ? C_PANEL2 : Color(14, 255, 255, 255));
        strokeRound(g, r, 9, C_LINE, 1.0f);
        Color c = on ? Color(200, 236, 234, 245) : Color(50, 236, 234, 245);
        Pen pen(c, 1.8f);
        float cx = r.X + r.Width / 2, cyy = r.Y + r.Height / 2;
        if (right) {
            drawLn(g, &pen, cx - 3, cyy - 6, cx + 3, cyy);
            drawLn(g, &pen, cx + 3, cyy, cx - 3, cyy + 6);
        } else {
            drawLn(g, &pen, cx + 3, cyy - 6, cx - 3, cyy);
            drawLn(g, &pen, cx - 3, cyy, cx + 3, cyy + 6);
        }
    };
    navBtn(prev, false);
    navBtn(next, true);

    const wchar_t* wd[7] = { L"一", L"二", L"三", L"四", L"五", L"六", L"日" };
    // 7 列铺满卡片内宽：46*7 + 8*6 = 370，两侧各留 27
    const float cell = HEAT_CELL, gap = HEAT_GAP, step = HEAT_STEP;
    float gridW = 7 * step - gap;
    float gx = hc.X + (hc.Width - gridW) / 2;
    float gy = hc.Y + 80;
    int rows = heatRows();

    for (int i = 0; i < 7; i++) {
        textAt(g, wd[i], RectF(gx + i * step, gy - 24, cell, 18), *fSmall, C_DIMMER);
    }

    int daysWithData = 0;
    long long monthTotal = 0;
    int bestDay = 0;
    long long bestMs = 0;

    for (int d = 1; d <= daysInMonth; d++) {
        int idx = startCol + d - 1;
        int r = idx / 7, c = idx % 7;
        float x = gx + c * step;
        float y = gy + r * step;

        long long ms = dayMs(d);
        if (ms > 0) {
            daysWithData++;
            monthTotal += ms;
            if (ms > bestMs) { bestMs = ms; bestDay = d; }
        }

        bool future = isThisMonth && d > base.tm_mday;   // 还没到的日子

        fillRound(g, RectF(x, y, cell, cell), 8.0f,
                  future ? Color(255, 24, 23, 38) : heatColor(ms));
        if (ms <= 0) strokeRound(g, RectF(x, y, cell, cell), 8.0f, Color(18, 255, 255, 255), 1.0f);

        if (isThisMonth && d == base.tm_mday) {
            strokeRound(g, RectF(x - 3, y - 3, cell + 6, cell + 6), 11.0f, Color(235, 255, 255, 255), 2.0f);
        }
        // 日期数字：格子够大，直接写进去
        wchar_t dn[8];
        swprintf_s(dn, L"%d", d);
        Color tc;
        if (future)                      tc = Color(55, 236, 234, 245);
        else if (ms >= 60 * 60000LL)     tc = Color(255, 10, 40, 52);
        else if (ms > 0)                 tc = Color(235, 250, 248, 255);
        else                             tc = Color(110, 236, 234, 245);
        textAt(g, dn, RectF(x, y, cell, cell), *fBody, tc);
    }

    // 底部汇总
    float fy = gy + rows * step - gap + 16;
    wchar_t b1[160];
    if (daysWithData > 0) {
        long long bm = bestMs / 60000;
        if (bm >= 60 && bm % 60 == 0) {
            swprintf_s(b1, L"本月专注 %d 天 · 共 %lld 小时 %lld 分 · 最长一天 %d 号 %lld 小时",
                      daysWithData, monthTotal / 3600000, (monthTotal % 3600000) / 60000,
                      bestDay, bm / 60);
        } else if (bm >= 60) {
            swprintf_s(b1, L"本月专注 %d 天 · 共 %lld 小时 %lld 分 · 最长一天 %d 号 %lld 小时 %lld 分",
                      daysWithData, monthTotal / 3600000, (monthTotal % 3600000) / 60000,
                      bestDay, bm / 60, bm % 60);
        } else {
            swprintf_s(b1, L"本月专注 %d 天 · 共 %lld 小时 %lld 分 · 最长一天 %d 号 %lld 分",
                      daysWithData, monthTotal / 3600000, (monthTotal % 3600000) / 60000,
                      bestDay, bm);
        }
    } else {
        swprintf_s(b1, L"这个月还没有记录");
    }
    textAt(g, b1, RectF(hc.X + 27, fy, hc.Width - 54, 20), *fSmall, Color(190, 236, 234, 245), StringAlignmentNear);

    // 色阶图例：五档色块 + 各自下方的说明
    float ly = fy + 30;
    const wchar_t* lb[5] = { L"没学", L"15 分", L"30 分", L"1 小时", L"2 小时+" };
    long long samples[5] = { 0, 5 * 60000LL, 20 * 60000LL, 45 * 60000LL, 90 * 60000LL };
    for (int i = 0; i < 5; i++) {
        float bx = gx + i * step + (cell - 24) / 2;
        fillRound(g, RectF(bx, ly, 24, 24), 6.0f, heatColor(samples[i]));
        if (samples[i] <= 0) strokeRound(g, RectF(bx, ly, 24, 24), 6.0f, Color(18, 255, 255, 255), 1.0f);
        textAt(g, lb[i], RectF(gx + i * step - 14, ly + 28, cell + 28, 16), *fTiny,
               Color(150, 236, 234, 245));
    }
    textAt(g, L"颜色越亮 = 那天学得越久", RectF(hc.X + 27, ly + 48, hc.Width - 54, 16), *fTiny,
           Color(70, 236, 234, 245), StringAlignmentNear);
}

static void drawToast(Graphics& g) {
    if (rt.toastText.empty()) return;
    long long now = nowMs();
    if (now > rt.toastUntil) { rt.toastText.clear(); return; }
    Font f(L"Microsoft YaHei UI", 12.0f, FontStyleRegular, UnitPixel);
    RectF meas;
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    g.MeasureString(rt.toastText.c_str(), -1, &f, PointF(0, 0), &sf, &meas);
    float w = meas.Width + 32;
    RectF r((CW - w) / 2, 118, w, 34);
    fillRound(g, r, 10, Color(240, 20, 20, 30));
    strokeRound(g, r, 10, C_LINE, 1.0f);
    textAt(g, rt.toastText, r, f, C_TEXT);
}

// ---------------- 绘制：主入口 ----------------
void drawSettingsPage(Graphics& g, float t);

static int g_paintN = 0;
static int g_timerN = 0;

static long long g_paintSum = 0;
static void renderAll(Graphics& g) {
    g.SetCompositingMode(CompositingModeSourceCopy);
    SolidBrush clear(Color(255, 7, 7, 12));
    fillRect(g, &clear, 0, 0, (float)CW, (float)CH);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    GraphicsState gs = g.Save();
    g.ScaleTransform(S, S);
    float t = (float)GetTickCount64() / 1000.0f;
    drawBackground(g);
    if (rt.page == 0) {
        // 先定死滚动上限，保证本帧用的是有效偏移
        rt.mainMax = max(0.0f, HEAT_TOP + heatCardH() + 20.0f - CH);
        if (rt.mainY > rt.mainMax) rt.mainY = rt.mainMax;
        if (rt.mainY < 0) rt.mainY = 0;

        // 整页滚动：顶栏固定，其余内容一起上移
        GraphicsState gsBody = g.Save();
        g.SetClip(RectF(0, TITLE_H, (float)CW, CH - TITLE_H));
        g.TranslateTransform(0, -rt.mainY);
        drawRing(g, t);
        drawStatus(g);
        drawButtons(g);
        drawStats(g);
        g.Restore(gsBody);
        drawStreamBar(g);
        drawTopBar(g, t);
    } else {
        drawSettingsPage(g, t);
    }
    drawToast(g);
    g.Restore(gs);
}

// 导出 PNG 用于验证与预览
static int getPngEncoder(CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
    GetImageEncoders(num, size, info);
    int found = -1;
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { *clsid = info[i].Clsid; found = i; break; }
    }
    free(info);
    return found;
}

// 高倍渲染后降采样：既保留细节，又不会在网页上变成一张占满屏幕的巨图
static void saveScaled(Bitmap& src, const wchar_t* path, int outW) {
    CLSID png;
    if (getPngEncoder(&png) < 0) return;
    if (outW <= 0 || outW >= src.GetWidth()) { src.Save(path, &png, nullptr); return; }
    int sw = src.GetWidth(), sh = src.GetHeight();
    int outH = (int)((long long)sh * outW / sw);
    Bitmap out(outW, outH, PixelFormat32bppPARGB);
    {
        Graphics g(&out);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.DrawImage(&src, Rect(0, 0, outW, outH), 0, 0, sw, sh, UnitPixel);
    }
    out.Save(path, &png, nullptr);
}

static void exportPng(const wchar_t* path, int outW = 0) {
    int w = (int)(CW * S), h = (int)(CH * S);
    Bitmap bmp(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&bmp);
        renderAll(g);
    }
    saveScaled(bmp, path, outW);
}

static void paint(HWND hwnd) {
    long long t0 = GetTickCount64();
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) { EndPaint(hwnd, &ps); return; }

    // 缓存内存 DC + 位图：BitBlt 比 GDI+ DrawImage 快一个数量级
    static HDC memDC = nullptr;
    static HBITMAP memBmp = nullptr;
    static HGDIOBJ memOld = nullptr;
    static int memW = 0, memH = 0;
    if (!memBmp || memW != w || memH != h) {
        HDC screen = GetDC(nullptr);
        if (!memDC) memDC = CreateCompatibleDC(screen);
        if (memBmp) {
            SelectObject(memDC, memOld);
            DeleteObject(memBmp);
        }
        memBmp = CreateCompatibleBitmap(screen, w, h);
        ReleaseDC(nullptr, screen);
        memOld = SelectObject(memDC, memBmp);
        memW = w; memH = h;
    }

    {
        Graphics g(memDC);
        renderAll(g);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);

    g_paintN++;
    g_paintSum += GetTickCount64() - t0;
}

// ============================================================
// 设置面板
// ============================================================
enum SType { ST_GROUP, ST_CARD, ST_SLIDER, ST_CHIPS, ST_SWITCH, ST_LINK, ST_HINT };

struct SItem {
    SType type;
    RectF r;
    RectF track;      // 滑块轨道
    RectF minusR, plusR;   // 左右加减按钮
    int id = ID_NONE;
    wstring title, sub, value;
    int chipIndex = 0;
    bool chipSelected = false;
};

static vector<SItem> g_items;
static float g_contentH = 0;

static void buildSettings() {
    g_items.clear();
    float x = 16, w = CW - 32;
    float y = 68;

    auto group = [&](const wchar_t* t) {
        SItem it; it.type = ST_GROUP; it.r = RectF(x + 2, y, w - 4, 16); it.title = t;
        g_items.push_back(it); y += 22;
    };
    auto card = [&](const wchar_t* title, const wchar_t* sub, const wchar_t* val, float hgt, int id) {
        SItem it; it.type = ST_CARD; it.r = RectF(x, y, w, hgt); it.id = id;
        it.title = title; it.sub = sub; it.value = val;
        g_items.push_back(it); y += hgt + 8;
    };
    // 一根滑杆行：标题 [−] 轨道 [+] 数值
    auto sliderRow = [&](int id, const wchar_t* title, const wstring& val, RectF card, float ry) {
        SItem s;
        s.type = ST_SLIDER; s.id = id; s.title = title; s.value = val;
        s.r      = RectF(card.X, ry, card.Width, 30);
        s.minusR = RectF(card.X + 72, ry + 2, 26, 26);
        s.track  = RectF(card.X + 104, ry + 13, card.Width - 238, 4);
        s.plusR  = RectF(card.X + card.Width - 130, ry + 2, 26, 26);
        g_items.push_back(s);
    };
    auto hint = [&](const wchar_t* t, float hgt) {
        SItem it; it.type = ST_HINT; it.r = RectF(x + 2, y, w - 4, hgt); it.title = t;
        g_items.push_back(it); y += hgt + 6;
    };
    auto sw = [&](const wchar_t* title, const wchar_t* sub, int id, bool on) {
        SItem it; it.type = ST_SWITCH; it.r = RectF(x, y, w, 52); it.id = id;
        it.title = title; it.sub = sub; it.chipSelected = on;
        g_items.push_back(it); y += 60;
    };
    auto link = [&](const wchar_t* title, const wchar_t* sub, int id) {
        SItem it; it.type = ST_LINK; it.r = RectF(x, y, w, 52); it.id = id;
        it.title = title; it.sub = sub;
        g_items.push_back(it); y += 60;
    };

    // 节奏
    group(L"节奏");
    {
        float ch = (st.mode == 0) ? 132.0f : 92.0f;
        int lo = min(st.minGap, st.maxGap), hi = max(st.minGap, st.maxGap);
        wstring t = (st.mode == 0)
            ? (L"每 " + to_wstring(lo) + L" ~ " + to_wstring(hi) + L" 分钟随机一次")
            : (L"每 " + to_wstring(st.fixedGap) + L" 分钟响一次");
        card(t.c_str(), L"", L"", ch, ID_NONE);
        // 覆盖为自定义绘制
        g_items.back().type = ST_CARD;
        g_items.back().chipIndex = (st.mode == 0) ? 1 : 0;
    }

    if (st.mode == 0) {
        // 两个滑块叠在卡片里
        RectF last = g_items.back().r;
        sliderRow(ID_R_MIN, L"最短", to_wstring(st.minGap) + L" 分", last, last.Y + 34);
        sliderRow(ID_R_MAX, L"最长", to_wstring(st.maxGap) + L" 分", last, last.Y + 68);

        SItem h; h.type = ST_HINT; h.title = L"系统在区间内随机挑一个时刻响铃，不告诉你具体是哪个。";
        h.r = RectF(last.X + 12, last.Y + 108, last.Width - 24, 16);
        g_items.push_back(h);
    } else {
        RectF last = g_items.back().r;
        sliderRow(ID_R_FIXED, L"间隔", to_wstring(st.fixedGap) + L" 分", last, last.Y + 38);
    }
    y += 4;

    // 专注周期
    group(L"专注周期");
    sw(L"到点提醒休息", L"专注一段时间后响一声，提醒你起身", ID_SW_CYCLE, st.cycleOn);
    if (st.cycleOn) {
        wstring t = L"专注 " + to_wstring(st.focusCyc) + L" 分 · 休息 " + to_wstring(st.breakCyc) + L" 分";
        card(t.c_str(), L"", L"", 124.0f, ID_NONE);
        RectF cr = g_items.back().r;

        sliderRow(ID_R_CYC_FOCUS, L"专注", to_wstring(st.focusCyc) + L" 分", cr, cr.Y + 34);
        sliderRow(ID_R_CYC_BREAK, L"休息", to_wstring(st.breakCyc) + L" 分", cr, cr.Y + 66);

        SItem h; h.type = ST_HINT;
        h.title = L"休息期间不响盲盒提示音，也不计入专注时长。";
        h.r = RectF(cr.X + 14, cr.Y + 100, cr.Width - 28, 18);
        g_items.push_back(h);
    }

    // 提示音
    group(L"提示音");
    {
        SItem it; it.type = ST_CHIPS; it.r = RectF(x, y, w, 0);
        float cw = (w - 14) / 3.0f, chh = 34;
        for (int i = 0; i < 12; i++) {
            int col = i % 3, row = i / 3;
            SItem c; c.type = ST_CHIPS; c.id = ID_SOUND_BASE + i;
            c.r = RectF(x + col * (cw + 7), y + row * (chh + 7), cw, chh);
            c.title = vf::soundName(i);
            c.chipSelected = st.enabled[i];
            g_items.push_back(c);
        }
        y += 4 * (34 + 7) + 2;
    }
    hint(L"单击试听，再单击开关是否参与随机。", 18);
    y += 2;

    // 音量
    {
        SItem it; it.type = ST_CARD; it.r = RectF(x, y, w, 58); it.id = ID_NONE;
        g_items.push_back(it);
        RectF cr = g_items.back().r;
        sliderRow(ID_R_VOL, L"音量", to_wstring((int)(st.volume * 100)) + L"%", cr, cr.Y + 14);
        y += 66;
    }

    // 环境音
    group(L"环境音");
    {
        float cw = (w - 21) / 4.0f;
        const wchar_t* names[4] = { L"关", L"雨声", L"棕噪", L"低频" };
        for (int i = 0; i < 4; i++) {
            SItem c; c.type = ST_CHIPS; c.id = ID_AMB_BASE + i;
            c.r = RectF(x + i * (cw + 7), y, cw, 34);
            c.title = names[i];
            c.chipSelected = (st.ambient == i);
            g_items.push_back(c);
        }
        y += 42;
        SItem it; it.type = ST_CARD; it.r = RectF(x, y, w, 58); it.id = ID_NONE; it.title = L"";
        g_items.push_back(it);
        RectF cr = g_items.back().r;
        sliderRow(ID_R_AMB, L"环境音量", to_wstring((int)(st.ambVolume * 100)) + L"%", cr, cr.Y + 14);
        y += 66;
    }

    // 提醒方式
    group(L"提醒方式");
    sw(L"心流卡片", L"响铃时弹一张提示卡，点任意处关闭", ID_SW_CARD, st.cardOn);
    sw(L"系统通知", L"窗口在后台时用 Windows 气泡提醒", ID_SW_NOTIFY, st.notifyOn);
    sw(L"窗口置顶", L"压在别的窗口上，摆桌角余光就能看到", ID_SW_TOPMOST, st.topmost);
    sw(L"响铃时浮到前面", L"看书时不看屏幕，响了自己跳出来提醒", ID_SW_FRONT, st.bringFront);
    sw(L"开机自启", L"随系统启动，常驻托盘", ID_SW_AUTO, st.autoStart);

    // 数据
    group(L"数据");
    link(L"打开数据目录", L"记录以 ini 存在本地，不上传", ID_USERDATA);
    link(L"清空今天的数据", L"只重置今日，历史曲线保留", ID_RESET);

    SItem v; v.type = ST_HINT; v.title = L"NudgeBell v1.0 · 全部音效实时合成 · 关闭窗口后仍在托盘运行";
    v.r = RectF(x + 2, y, w - 4, 40);
    g_items.push_back(v);
    y += 44;

    g_contentH = y + 16;
}

void drawSettingsPage(Graphics& g, float t) {
    buildSettings();
    const float BODY_BOTTOM = CH - 56;          // 底部留给保存栏
    rt.scrollMax = max(0.0f, g_contentH - (BODY_BOTTOM - 56));
    if (rt.scrollY > rt.scrollMax) rt.scrollY = rt.scrollMax;

    // 背景
    LinearGradientBrush bg(PointF(0, 0), PointF(0, CH), Color(255, 20, 18, 31), Color(255, 10, 10, 17));
    bg.SetWrapMode(WrapModeClamp);
    fillRect(g, &bg, 0, 0, CW, CH);

    // 内容（裁剪 + 滚动）
    GraphicsState gs = g.Save();
    g.SetClip(RectF(0, 56, CW, BODY_BOTTOM - 56));
    GraphicsState gs2 = g.Save();
    g.TranslateTransform(0, -rt.scrollY);

    for (auto& it : g_items) {
        switch (it.type) {
        case ST_GROUP: {
            textAt(g, it.title, it.r, *fTiny, Color(110, 236, 234, 245), StringAlignmentNear);
            break;
        }
        case ST_CARD: {
            fillRound(g, it.r, 12, C_PANEL);
            strokeRound(g, it.r, 12, C_LINE, 1.0f);
            if (!it.title.empty()) {
                textAt(g, it.title, RectF(it.r.X + 16, it.r.Y + 8, it.r.Width - 32, 20), *fBody, C_TEXT, StringAlignmentNear);
            }
            break;
        }
        case ST_SLIDER: {
            if (!it.title.empty()) {
                textAt(g, it.title, RectF(it.r.X + 16, it.r.Y, 54, it.r.Height), *fSmall, Color(150, 236, 234, 245), StringAlignmentNear);
            }
            // 左右加减按钮
            {
                auto stepBtn = [&](RectF br, bool plus) {
                    int sid = plus ? ID_STEP_P + it.id : ID_STEP_M + it.id;
                    bool hov = (rt.hover == sid);
                    fillRound(g, br, 8, hov ? C_PANEL2 : Color(16, 255, 255, 255));
                    strokeRound(g, br, 8, hov ? Color(110, 124, 92, 255) : C_LINE, 1.0f);
                    Pen pen(hov ? Color(235, 255, 255, 255) : Color(185, 236, 234, 245), 1.7f);
                    float mx = br.X + br.Width / 2, my = br.Y + br.Height / 2;
                    drawLn(g, &pen, mx - 5, my, mx + 5, my);
                    if (plus) drawLn(g, &pen, mx, my - 5, mx, my + 5);
                };
                if (!it.minusR.IsEmptyArea()) stepBtn(it.minusR, false);
                if (!it.plusR.IsEmptyArea()) stepBtn(it.plusR, true);
            }
            // 轨道
            fillRound(g, it.track, 2, Color(34, 255, 255, 255));
            float p = 0;
            int val = 0;
            if (it.id == ID_R_MIN) { p = (st.minGap - 1) / 39.0f; }
            else if (it.id == ID_R_MAX) { p = (st.maxGap - 2) / 88.0f; }
            else if (it.id == ID_R_FIXED) { p = (st.fixedGap - 1) / 89.0f; }
            else if (it.id == ID_R_VOL) { p = st.volume; }
            else if (it.id == ID_R_AMB) { p = st.ambVolume; }
            else if (it.id == ID_R_CYC_FOCUS) { p = (st.focusCyc - 5) / 175.0f; }
            else if (it.id == ID_R_CYC_BREAK) { p = (st.breakCyc - 1) / 59.0f; }
            p = min(1.0f, max(0.0f, p));
            if (p > 0.01f) {
                RectF fill(it.track.X, it.track.Y, it.track.Width * p, it.track.Height);
                LinearGradientBrush lgb(PointF(fill.X, fill.Y), PointF(fill.X + fill.Width + 1, fill.Y), C_ACCENT, C_ACCENT2);
                lgb.SetWrapMode(WrapModeClamp);
                fillRound(g, fill, 2, Color(0, 0, 0, 0));
                GraphicsPath pp;
                addRoundRect(pp, fill, 2, 0.5f);
                g.FillPath(&lgb, &pp);
            }
            // 拇指
            float thx = it.track.X + it.track.Width * p;
            float thy = it.track.Y + it.track.Height / 2;
            SolidBrush halo(Color(60, 124, 92, 255));
            fillEll(g, &halo, thx - 7, thy - 7, 14, 14);
            SolidBrush thumb(Color(255, 255, 255, 255));
            fillEll(g, &thumb, thx - 4.5f, thy - 4.5f, 9, 9);

            if (!it.value.empty()) {
                textAt(g, it.value, RectF(it.r.GetRight() - 100, it.r.Y, 86, it.r.Height),
                       *fSmall, Color(184, 169, 255, 255), StringAlignmentFar);
            }
            break;
        }
        case ST_CHIPS: {
            bool sel = it.chipSelected;
            if (sel) {
                fillRound(g, it.r, 10, Color(28, 124, 92, 255));
                strokeRound(g, it.r, 10, Color(107, 124, 92, 255), 1.0f);
            } else {
                fillRound(g, it.r, 10, C_PANEL);
                strokeRound(g, it.r, 10, C_LINE, 1.0f);
            }
            textAt(g, it.title, it.r, *fSmall, sel ? Color(222, 214, 255, 255) : Color(120, 236, 234, 245));
            break;
        }
        case ST_SWITCH: {
            fillRound(g, it.r, 12, C_PANEL);
            strokeRound(g, it.r, 12, C_LINE, 1.0f);
            textAt(g, it.title, RectF(it.r.X + 16, it.r.Y + 9, 240, 20), *fBody, C_TEXT, StringAlignmentNear);
            textAt(g, it.sub, RectF(it.r.X + 16, it.r.Y + 28, 280, 16), *fTiny, C_DIMMER, StringAlignmentNear);
            // 开关
            RectF swr(it.r.GetRight() - 56, it.r.Y + 15, 40, 22);
            fillRound(g, swr, 11, it.chipSelected ? Color(255, 124, 92, 255) : Color(30, 255, 255, 255));
            if (it.chipSelected) {
                LinearGradientBrush lgb(PointF(swr.X, swr.Y), PointF(swr.X + swr.Width, swr.Y + swr.Height), C_ACCENT, C_ACCENT2);
                lgb.SetWrapMode(WrapModeClamp);
                GraphicsPath pp;
                addRoundRect(pp, swr, 11, 0.5f);
                g.FillPath(&lgb, &pp);
            }
            float kx = swr.X + (it.chipSelected ? 21 : 3);
            SolidBrush knob(Color(255, 255, 255, 255));
            fillEll(g, &knob, kx, swr.Y + 3, 16, 16);
            break;
        }
        case ST_LINK: {
            fillRound(g, it.r, 12, C_PANEL);
            strokeRound(g, it.r, 12, C_LINE, 1.0f);
            textAt(g, it.title, RectF(it.r.X + 16, it.r.Y + 9, 240, 20), *fBody, C_TEXT, StringAlignmentNear);
            textAt(g, it.sub, RectF(it.r.X + 16, it.r.Y + 28, 300, 16), *fTiny, C_DIMMER, StringAlignmentNear);
            Pen pen(Color(90, 236, 234, 245), 1.8f);
            float ax = it.r.GetRight() - 26, ay = it.r.Y + 26;
            drawLn(g, &pen, ax - 4, ay - 5, ax + 2, ay);
            drawLn(g, &pen, ax + 2, ay, ax - 4, ay + 5);
            break;
        }
        case ST_HINT: {
            textAt(g, it.title, it.r, *fTiny, Color(90, 236, 234, 245), StringAlignmentNear, StringAlignmentNear);
            break;
        }
        }
    }

    g.Restore(gs2);
    g.Restore(gs);

    // 顶栏
    LinearGradientBrush hb(PointF(0, 0), PointF(0, 56), Color(255, 22, 20, 34), Color(230, 16, 15, 26));
    hb.SetWrapMode(WrapModeClamp);
    fillRect(g, &hb, 0, 0, CW, 56);
    Pen hp(Color(24, 255, 255, 255), 1.0f);
    drawLn(g, &hp, 0, 55, CW, 55);

    textAt(g, L"设置", RectF(0, 0, CW, 56), *fTitle, C_TEXT);
    // 关闭按钮
    RectF cb(CW - 46, 13, 30, 30);
    fillRound(g, cb, 9, rt.hover == ID_SHEET_CLOSE ? C_PANEL2 : C_PANEL);
    strokeRound(g, cb, 9, C_LINE, 1.0f);
    {
        Pen pen(C_DIM, 1.8f);
        drawLn(g, &pen, cb.X + 10, cb.Y + 12, cb.X + 15, cb.Y + 18);
        drawLn(g, &pen, cb.X + 15, cb.Y + 18, cb.X + 20, cb.Y + 12);
    }

    // 底部保存栏
    {
        LinearGradientBrush bb(PointF(0, BODY_BOTTOM), PointF(0, CH), Color(240, 20, 18, 31), Color(255, 12, 11, 20));
        bb.SetWrapMode(WrapModeClamp);
        fillRect(g, &bb, 0, BODY_BOTTOM, CW, CH - BODY_BOTTOM);
        Pen bp(Color(26, 255, 255, 255), 1.0f);
        drawLn(g, &bp, 0, BODY_BOTTOM, CW, BODY_BOTTOM);

        textAt(g, rt.dirty ? L"有改动 · 还没保存" : L"设置已是最新",
               RectF(20, BODY_BOTTOM, 200, CH - BODY_BOTTOM), *fSmall,
               rt.dirty ? Color(200, 255, 196, 120) : Color(80, 236, 234, 245),
               StringAlignmentNear);

        RectF sb = rSaveBtn();
        if (rt.dirty) {
            LinearGradientBrush lg(PointF(sb.X, sb.Y), PointF(sb.X + sb.Width, sb.Y + sb.Height), C_ACCENT, C_ACCENT2);
            lg.SetWrapMode(WrapModeClamp);
            GraphicsPath pp;
            addRoundRect(pp, sb, 11, 0.5f);
            g.FillPath(&lg, &pp);
            LinearGradientBrush hi(PointF(0, sb.Y), PointF(0, sb.Y + sb.Height), Color(52, 255, 255, 255), Color(0, 255, 255, 255));
            hi.SetWrapMode(WrapModeClamp);
            g.FillPath(&hi, &pp);
        } else {
            fillRound(g, sb, 11, Color(16, 255, 255, 255));
            strokeRound(g, sb, 11, C_LINE, 1.0f);
        }
        textAt(g, L"保存设置", sb, *fSmall,
               rt.dirty ? Color(255, 255, 255, 255) : Color(95, 236, 234, 245));
    }

    // 滚动条
    float viewH = BODY_BOTTOM - 56;
    if (rt.scrollMax > 1) {
        float frac = viewH / g_contentH;
        float barH = max(30.0f, viewH * frac);
        float y = 56 + (rt.scrollY / rt.scrollMax) * (viewH - barH);
        fillRound(g, RectF(CW - 5, y, 3, barH), 1.5f, Color(40, 255, 255, 255));
    }
}

// ============================================================
// 心流卡片
// ============================================================
// 按像素宽度断行（GDI+ 的 DrawString 在给定高度内不会自动换行，需手动切分）
static vector<wstring> wrapText(Graphics& g, Font& f, const wstring& s, float maxW) {
    vector<wstring> out;
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);

    RectF all;
    g.MeasureString(s.c_str(), -1, &f, PointF(0, 0), &sf, &all);
    int n = (int)ceilf(all.Width / maxW);
    if (n < 1) n = 1;
    // 按均分宽度断行，避免末行只剩一两个字
    float target = min(maxW, all.Width / n * 1.12f);

    wstring cur;
    for (size_t i = 0; i < s.size(); i++) {
        wstring test = cur + s[i];
        RectF b;
        g.MeasureString(test.c_str(), -1, &f, PointF(0, 0), &sf, &b);
        if (b.Width > target && !cur.empty()) {
            out.push_back(cur);
            cur = s[i];
        } else {
            cur = test;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ============================================================
// 心流卡片：独立的置顶浮窗（分层窗口，带阴影、圆角、真透明）
// ============================================================
static const float CARD_W = 380.0f;     // 卡片逻辑宽
static const float CARD_PAD = 22.0f;    // 画布四周留白，用来放阴影
static HWND g_cardHwnd = nullptr;
static long long g_cardPaintAt = 0;
static int g_cardShowN = 0;              // 一共弹过几次卡片

// 卡片要多高，先算出来再定窗口尺寸
static float cardBodyHeight() {
    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics g(&probe);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    Font textF(L"Microsoft YaHei UI", 19.0f, FontStyleRegular, UnitPixel);
    vector<wstring> lines = wrapText(g, textF, rt.cardText, CARD_W - 44);
    float textH = max(32.0f, (float)lines.size() * 32.0f);
    return 46 + 22 + textH + 26 + 34;
}

// 把卡片画到透明画布上，(0,0) 是画布左上角
static void drawCardBody(Graphics& g, float bodyH) {
    RectF card(CARD_PAD, CARD_PAD, CARD_W, bodyH);

    // 柔和阴影：多层递减，叠出羽化边
    for (int i = 7; i >= 1; i--) {
        RectF sh(card.X - i * 2.4f, card.Y - i * 1.4f + 8, card.Width + i * 4.8f, card.Height + i * 2.8f);
        SolidBrush sb(Color(10, 0, 0, 0));
        GraphicsPath sp;
        addRoundRect(sp, sh, 20 + i * 2.4f, 0.5f);
        g.FillPath(&sb, &sp);
    }

    GraphicsPath p;
    addRoundRect(p, card, 20, 0.5f);
    LinearGradientBrush lgb(PointF(card.X, card.Y), PointF(card.X + card.Width, card.Y + card.Height),
                            Color(252, 62, 48, 120), Color(252, 22, 46, 74));
    lgb.SetWrapMode(WrapModeClamp);
    g.FillPath(&lgb, &p);
    strokeRound(g, card, 20, Color(56, 255, 255, 255), 1.0f);

    Font textF(L"Microsoft YaHei UI", 19.0f, FontStyleRegular, UnitPixel);
    vector<wstring> lines = wrapText(g, textF, rt.cardText, card.Width - 44);

    textAt(g, rt.cardKind, RectF(card.X, card.Y + 22, card.Width, 16), *fTiny, Color(140, 255, 255, 255));
    textAt(g, rt.cardSound, RectF(card.X, card.Y + 44, card.Width, 18), *fSmall, Color(255, 165, 236, 255));

    for (size_t i = 0; i < lines.size(); i++) {
        textAt(g, lines[i], RectF(card.X + 22, card.Y + 68 + i * 32.0f, card.Width - 44, 32.0f),
               textF, Color(255, 255, 255, 255));
    }

    wstring meta = rt.cardManual
        ? L"随机音色试听 · 今日已专注 " + fmtClock(rt.cardFocus)
        : L"已专注 " + fmtClock(rt.cardFocus) + L" · 第 " + to_wstring(rt.cardBells) + L" 次唤醒";
    float textH = max(32.0f, (float)lines.size() * 32.0f);
    textAt(g, meta, RectF(card.X, card.Y + 68 + textH + 8, card.Width, 18), *fTiny, Color(120, 255, 255, 255));

    // 底部进度条：剩余时间
    RectF bar(card.X + 22, card.GetBottom() - 22, card.Width - 44, 2);
    fillRound(g, bar, 1, Color(30, 255, 255, 255));
    long long now = nowMs();
    long long span = max(1LL, rt.cardUntil - rt.cardStartAt);
    float frac = max(0.0f, min(1.0f, (float)(rt.cardUntil - now) / (float)span));
    if (frac > 0.01f) {
        RectF fill(bar.X, bar.Y, bar.Width * frac, 2);
        LinearGradientBrush bg(PointF(fill.X, fill.Y), PointF(fill.X + fill.Width + 1, fill.Y), C_ACCENT, C_ACCENT2);
        bg.SetWrapMode(WrapModeClamp);
        GraphicsPath bp;
        addRoundRect(bp, fill, 1, 1.0f);
        g.FillPath(&bg, &bp);
    }
}

static void paintCardWindow() {
    if (!g_cardHwnd) return;
    float bodyH = cardBodyHeight();
    int w = (int)((CARD_W + CARD_PAD * 2) * S);
    int h = (int)((bodyH + CARD_PAD * 2) * S);
    if (w <= 0 || h <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // 从上往下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldObj = SelectObject(mem, dib);
    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.ScaleTransform(S, S);
        drawCardBody(g, bodyH);
    }

    // 弹在主窗口所在的那块屏幕上，多显示器时不会飞到别处
    HMONITOR mon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFO mi = { sizeof(MONITORINFO) };
    RECT work;
    if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT dst = { work.left + (work.right - work.left - w) / 2,
                  work.top + (int)((work.bottom - work.top) * 0.28f) };
    SIZE sz = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_cardHwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, oldObj);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK CardProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        rt.cardVisible = false;
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;      // 不抢焦点，别打断正在打字的人
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(h, msg, w, l);
}

static void ensureCardWindow() {
    if (g_cardHwnd) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CardProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        wc.lpszClassName = L"NudgeBellCard";
        RegisterClassExW(&wc);
        reg = true;
    }
    g_cardHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"NudgeBellCard", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_hInst, nullptr);
}

static void showCardPopup() {
    ensureCardWindow();
    if (!g_cardHwnd) return;
    paintCardWindow();
    ShowWindow(g_cardHwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_cardHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_cardPaintAt = nowMs();
}

static void hideCardPopup() {
    if (g_cardHwnd) ShowWindow(g_cardHwnd, SW_HIDE);
}

// 把桌面某块矩形真实像素抓下来（用于验证浮窗确实合成到了屏幕上）
static void captureRect(const RECT& r, const wchar_t* path) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ oldObj = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    {
        Bitmap gbmp(bmp, nullptr);
        CLSID png;
        if (getPngEncoder(&png) >= 0) gbmp.Save(path, &png, nullptr);
    }
    SelectObject(mem, oldObj);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void exportCardPng(const wchar_t* path, int outW = 0) {
    float bodyH = cardBodyHeight();
    int w = (int)((CARD_W + CARD_PAD * 2) * S);
    int h = (int)((bodyH + CARD_PAD * 2) * S);
    Bitmap bmp(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&bmp);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.ScaleTransform(S, S);
        drawCardBody(g, bodyH);
    }
    saveScaled(bmp, path, outW);
}


// ============================================================
// 业务逻辑
// ============================================================
static const wchar_t* LINE_KIND[5] = { L"深呼吸", L"身体", L"进度自检", L"觉察", L"鼓励" };
static const wchar_t* LINES[5][5] = {
    { L"吸 4 秒，停 2 秒，呼 6 秒。就一轮。", L"先把这口气吐干净，再回去。",
      L"肩膀放下来一点，你刚才一直提着。", L"下巴微收，让后颈松一点。", L"现在这一口气，慢一半。" },
    { L"站起来，走到窗前，看远处 20 秒。", L"喝一口水。真的去，别应付。",
      L"眼睛闭 10 秒，让视线从屏幕上撤回来。", L"手腕转两圈，脖子向右慢慢偏一下。",
      L"把背靠到椅背上，让脊柱自己撑住。" },
    { L"刚才这一段，你搞懂了什么？用一句话说给自己听。", L"如果现在必须停，下次怎么接上？写下那个断点。",
      L"卡住的地方先记下来，别在这儿耗。", L"把刚冒出来的想法写三个字，别让它跑了。",
      L"现在做的事，是必要的，还是舒服的？" },
    { L"刚才走神去哪儿了？看一眼，然后回来。", L"脑子里还悬着一件事？写下来，然后放下它。",
      L"注意一下呼吸——是不是有点浅了。", L"你不是在坚持，你只是还没离开。", L"安静地继续，不需要给谁看。" },
    { L"你已经在里面了，这就够了。", L"不用等状态好才开始，开始了状态才来。",
      L"这一轮不用多，只要别断。", L"刚才那阵走神很正常。回来就好。", L"把注意力放回手指和键盘之间。" }
};

static void toast(const wstring& s, int ms = 2000) {
    rt.toastText = s;
    rt.toastUntil = nowMs() + ms;
}

static void raiseWindow();
static void trayBubble(const wstring& title, const wstring& body, DWORD flag);

static void showCard(int kindIdx, int lineIdx, int soundIdx, bool manual) {
    rt.cardKind = manual ? L"试听" : LINE_KIND[kindIdx];
    rt.cardText = LINES[kindIdx][lineIdx];
    rt.cardSound = vf::soundName(soundIdx);
    rt.cardFocus = st.focusMs;
    rt.cardBells = st.bells;
    rt.cardManual = manual;
    rt.cardStartAt = nowMs();
    rt.cardUntil = rt.cardStartAt + 6000;
    rt.cardVisible = true;
    g_cardShowN++;
    showCardPopup();
}

static void doRing(bool manual) {
    int pool[12], n = 0;
    for (int i = 0; i < 12; i++) if (st.enabled[i]) pool[n++] = i;
    if (n == 0) { for (int i = 0; i < 12; i++) pool[n++] = i; }
    int sid = pool[rand() % n];

    // 缓存音色样本
    static Buf cache[12];
    static bool loaded[12] = {false};
    if (!loaded[sid]) { cache[sid] = vf::makeSound(sid); loaded[sid] = true; }
    if (st.soundOn) vf::playOnce(cache[sid], st.volume);

    rt.flash = 1.0f;
    if (!manual) {
        st.bells++;
        rt.lastBellAt = nowMs();
    }

    int kindIdx = rand() % 5;
    int lineIdx = rand() % 5;

    if (st.cardOn) showCard(kindIdx, lineIdx, sid, manual);
    else if (!manual) toast(wstring(vf::soundName(sid)) + L" · " + LINES[kindIdx][lineIdx], 4000);

    // 卡片已经独立浮在最前面了；只有关掉卡片、只剩 toast 时才把主窗口带出来
    if (st.bringFront && !manual && !st.cardOn && rt.page == 0) raiseWindow();

    if (st.notifyOn && !IsWindowVisible(g_hwnd)) {
        trayBubble(wstring(vf::soundName(sid)) + L" · " + (manual ? wstring(L"试听") : wstring(LINE_KIND[kindIdx])),
                   LINES[kindIdx][lineIdx], NIIF_NONE);
    }
}

static void scheduleNext(long long from) {
    rt.lastBellAt = from;
    rt.nextBellAt = from + nextGapMs();
}

// ============================================================
// 专注周期：专注 N 分 → 提醒休息 M 分 → 回到专注
// ============================================================
// 进入某个阶段：专注段恢复盲盒排期，休息段彻底静音
static void enterPhase(int p, long long now) {
    rt.phase = p;
    rt.phaseStart = now;
    rt.phaseEnd = now + (long long)(p == 1 ? st.breakCyc : st.focusCyc) * 60000LL;
    if (p == 0) {
        scheduleNext(now);
    } else {
        rt.lastBellAt = now;
        rt.nextBellAt = rt.phaseEnd + 1;     // 休息期间不会触发盲盒
    }
}

// 把窗口推到最前（Windows 前台锁定下必须短暂置顶）
static void raiseWindow() {
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    if (GetForegroundWindow() != g_hwnd) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(g_hwnd);
        SetWindowPos(g_hwnd, st.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
    }
}

static void trayBubble(const wstring& title, const wstring& body, DWORD flag) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, title.c_str());
    wcscpy_s(nid.szInfo, body.c_str());
    nid.dwInfoFlags = flag;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void startBreak(long long now) {
    enterPhase(1, now);
    st.breaks++;
    rt.flash = 1.0f;

    // 用一个固定音色，跟盲盒的随机音区分开
    static Buf bc;
    static bool bl = false;
    if (!bl) { bc = vf::makeSound(0); bl = true; }
    if (st.soundOn) vf::playOnce(bc, st.volume);

    wstring body = L"专注 " + to_wstring(st.focusCyc) + L" 分钟到。先闭眼 10 秒，"
                   L"再站起来走一走、看看远处，" + to_wstring(st.breakCyc) + L" 分钟后回来。";
    if (st.cardOn) {
        rt.cardKind = L"该休息了";
        rt.cardText = body;
        rt.cardSound = vf::soundName(0);
        rt.cardFocus = st.focusMs;
        rt.cardBells = st.bells;
        rt.cardManual = false;
        rt.cardStartAt = now;
        rt.cardUntil = now + 14000;
        rt.cardVisible = true;
        showCardPopup();
    } else {
        toast(L"该休息 " + to_wstring(st.breakCyc) + L" 分钟了", 6000);
    }
    if (st.bringFront && !st.cardOn && rt.page == 0) raiseWindow();
    if (st.notifyOn && !IsWindowVisible(g_hwnd)) trayBubble(L"NudgeBell · 该休息了", body, NIIF_INFO);
    saveState();
}

static void endBreak(long long now, bool skipped) {
    enterPhase(0, now);
    rt.flash = 0.6f;

    static Buf bc;
    static bool bl = false;
    if (!bl) { bc = vf::makeSound(1); bl = true; }
    if (st.soundOn) vf::playOnce(bc, st.volume * 0.8f);

    toast(skipped ? L"跳过休息 · 接着来" : L"休息结束 · 接着来", 3500);
    if (st.notifyOn && !IsWindowVisible(g_hwnd))
        trayBubble(L"NudgeBell · 继续专注", L"休息结束，回来接着来。", NIIF_NONE);
    saveState();
}

static void startSession() {
    long long now = nowMs();
    if (st.sessionStart == 0) st.sessionStart = now;
    rt.running = true;
    st.paused = false;
    rt.phase = 0;
    rt.phaseStart = now;
    rt.phaseEnd = now + (long long)st.focusCyc * 60000LL;
    rt.phaseLeftMs = 0;
    scheduleNext(now);
    rt.lastBellAt = now;
    saveState();
}

static void pauseSession() {
    long long now = nowMs();
    rt.running = false;
    st.paused = true;
    rt.remainingMs = max(3000LL, rt.nextBellAt - now);
    rt.phaseLeftMs = st.cycleOn ? max(1000LL, rt.phaseEnd - now) : 0;
    saveState();
    toast(rt.phase == 1 ? L"已暂停 · 休息也停住了" : L"已暂停 · 盲盒也停住了");
}

static void resumeSession() {
    long long now = nowMs();
    rt.running = true;
    st.paused = false;
    if (rt.phase == 1) {
        rt.phaseStart = now - (phaseTotalMs() - rt.phaseLeftMs);
        rt.phaseEnd = now + max(1000LL, rt.phaseLeftMs);
        rt.nextBellAt = rt.phaseEnd + 1;
    } else {
        rt.nextBellAt = now + max(3000LL, rt.remainingMs > 0 ? rt.remainingMs : 60000);
        if (st.cycleOn) {
            if (rt.phaseLeftMs > 0) {
                rt.phaseStart = now - (phaseTotalMs() - rt.phaseLeftMs);
                rt.phaseEnd = now + rt.phaseLeftMs;
            } else {
                // 重启后是全新的一轮，phaseEnd 不能留在 0，否则周期永远不触发
                rt.phaseStart = now;
                rt.phaseEnd = now + phaseTotalMs();
            }
        }
    }
    saveState();
}

// 上次退出时是暂停的；重启后也只停在暂停态，等你点了才继续
static void restoreSession() {
    if (!st.sessionStart) return;
    rt.running = false;
    st.paused = true;
    rt.remainingMs = 60000;
    rt.phase = 0;
    rt.phaseLeftMs = 0;
    rt.cardVisible = false;
    saveState(true);
}

// 用户把 state.ini 删了，就该真的归零。
// 不这么干的话，进程会每 15 秒把内存里的旧状态写回去 —— 删档等于白删。
// 这是之前"删了配置文件数据还在"的真凶。
static bool g_stateResetDone = false;

// 数据文件没了，或者被清成 0 字节 —— 都算用户要清空
static bool stateFileGone() {
    if (g_stateFile.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(g_stateFile.c_str(), GetFileExInfoStandard, &fad)) return true;
    return fad.nFileSizeLow == 0;
}

static void resetStateIfFileDeleted() {
    if (g_testMode || g_stateFile.empty()) return;                       // 测试模式不碰
    bool gone = stateFileGone();
    if (!gone) {
        g_stateResetDone = false;
        return;                                          // 档还在，正常
    }
    if (g_stateResetDone) return;        // 只清一次：万一写盘失败，别把用户数据反复清空
    g_stateResetDone = true;

    bool keepAuto = st.autoStart;        // 开机自启是注册表副作用，不跟着数据一起清
    st = AppState();                     // 设置、统计、历史全部回默认
    st.autoStart = keepAuto;
    st.todayDate = todayKey();
    rt.running = false;
    rt.phase = 0;
    rt.phaseStart = rt.phaseEnd = 0;
    rt.phaseLeftMs = 0;
    rt.nextBellAt = rt.lastBellAt = 0;
    rt.remainingMs = 60000;
    rt.cardVisible = false;
    rt.cardManual = false;
    rt.viewMonth = 0;
    rt.scrollY = 0.0f;
    rt.mainY = 0.0f;
    rt.activeSlider = -1;
    rt.dirty = false;
    hideCardPopup();
    saveState(true);                     // 立刻写一份完整的默认档，别留个 0 字节文件在那
    toast(L"数据已清空，恢复默认设置");
}

// 退出前先落成"暂停"，下次打开不会接着跑
static void markPausedOnExit() {
    resetStateIfFileDeleted();           // 先看用户是不是删了档，别把旧数据又写回去
    rt.running = false;
    st.paused = true;
    rt.phase = 0;
    rt.phaseLeftMs = 0;
    rt.cardVisible = false;
    hideCardPopup();
    saveState(true);
}

static void resetToday() {
    ensureToday();
    st.focusMs = 0;
    st.bells = 0;
    st.breaks = 0;
    rt.running = false;
    rt.phase = 0;
    rt.phaseLeftMs = 0;
    st.sessionStart = 0;
    st.paused = false;
    saveState();
    toast(L"今天的数据已清空");
}

// ============================================================
// 命中测试
// ============================================================
static int hitTest(float x, float y) {

    if (rt.page == 1) {
        if (x >= CW - 46 && x <= CW - 16 && y >= 13 && y <= 43) return ID_SHEET_CLOSE;
        if (y < 56) return ID_NONE;
        if (rSaveBtn().Contains(x, y)) return ID_SAVE;     // 底部保存栏不跟随滚动
        float cy = y + rt.scrollY;
        for (auto& it : g_items) {
            if (it.id == ID_NONE) continue;      // 背景卡只负责画，不负责接点击
            if (it.type == ST_SLIDER) {
                // 先把 +/- 按钮吃掉，再判断轨道
                if (!it.minusR.IsEmptyArea() && it.minusR.Contains(x, cy)) return ID_STEP_M + it.id;
                if (!it.plusR.IsEmptyArea() && it.plusR.Contains(x, cy)) return ID_STEP_P + it.id;
                // 拖动区只限轨道那一段，点标题或数值不会误跳
                if (it.track.Width > 0 && !it.plusR.IsEmptyArea()) {
                    RectF band(it.track.X - 4, it.r.Y, it.plusR.X - it.track.X + 8, it.r.Height);
                    if (band.Contains(x, cy)) return it.id;
                }
            } else if (it.type == ST_CHIPS || it.type == ST_SWITCH || it.type == ST_LINK || it.type == ST_CARD) {
                if (it.r.Contains(x, cy)) return it.id;
            }
        }
        return ID_NONE;
    }

    if (rModeSeg().Contains(x, y)) return (x < rModeSeg().X + rModeSeg().Width / 2) ? ID_BLIND : ID_FIXED;
    if (rGear().Contains(x, y)) return ID_GEAR;
    if (rMin().Contains(x, y)) return ID_MIN;
    if (rClose().Contains(x, y)) return ID_CLOSE;
    float cy = y + rt.mainY;   // 内容整体上移了 mainY
    {
        float hyt = 560 + 64 + 12;          // 热力图卡片顶部
        float NAV = 34.0f;
        if (RectF(18 + 14, hyt + 12, NAV, 28).Contains(x, cy)) return ID_PREV_MONTH;
        if (RectF(18 + (CW - 36) - 14 - NAV, hyt + 12, NAV, 28).Contains(x, cy)) return ID_NEXT_MONTH;
    }
    if (rMainBtn().Contains(x, cy)) return ID_MAIN;
    if (rBell().Contains(x, cy)) return ID_BELL;
    if (rMute().Contains(x, cy)) return ID_MUTE;
    return ID_NONE;
}

static int sliderFromId(int id) { return id; }

// 点一次 +/- 走一档
static void nudgeSlider(int id, int dir) {
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    switch (id) {
    case ID_R_MIN:
        st.minGap = clampi(st.minGap + dir, 1, 39);
        if (st.minGap >= st.maxGap) st.maxGap = clampi(st.minGap + 1, 2, 90);
        break;
    case ID_R_MAX:
        st.maxGap = clampi(st.maxGap + dir, 2, 90);
        if (st.maxGap <= st.minGap) st.minGap = clampi(st.maxGap - 1, 1, 39);
        break;
    case ID_R_FIXED:
        st.fixedGap = clampi(st.fixedGap + dir, 1, 90);
        break;
    case ID_R_VOL: {
        float v = st.volume + dir * 0.05f;
        st.volume = min(1.0f, max(0.0f, v));
        st.soundOn = st.volume > 0.005f;
        break;
    }
    case ID_R_AMB:
        st.ambVolume = min(1.0f, max(0.0f, st.ambVolume + dir * 0.05f));
        vf::setAmbientVolume(st.ambVolume);
        break;
    case ID_R_CYC_FOCUS:
        st.focusCyc = clampi(st.focusCyc + dir * 5, 5, 180);
        if (rt.running && rt.phase == 0) rt.phaseEnd = rt.phaseStart + phaseTotalMs();
        break;
    case ID_R_CYC_BREAK:
        st.breakCyc = clampi(st.breakCyc + dir, 1, 60);
        if (rt.running && rt.phase == 1) rt.phaseEnd = rt.phaseStart + phaseTotalMs();
        break;
    default:
        return;
    }
    if ((id == ID_R_MIN || id == ID_R_MAX || id == ID_R_FIXED) && rt.running && rt.phase == 0)
        scheduleNext(nowMs());
    rt.dirty = true;
}

static void setSliderValue(int id, float x) {
    for (auto& it : g_items) {
        if (it.id != id || it.type != ST_SLIDER) continue;
        float p = (x - it.track.X) / max(1.0f, it.track.Width);
        p = min(1.0f, max(0.0f, p));
        if (id == ID_R_MIN) { st.minGap = (int)(1 + p * 39 + 0.5f); if (st.minGap >= st.maxGap) st.maxGap = min(90, st.minGap + 1); }
        else if (id == ID_R_MAX) { st.maxGap = (int)(2 + p * 88 + 0.5f); if (st.maxGap <= st.minGap) st.minGap = max(1, st.maxGap - 1); }
        else if (id == ID_R_FIXED) { st.fixedGap = (int)(1 + p * 89 + 0.5f); }
        else if (id == ID_R_VOL) { st.volume = p; st.soundOn = p > 0.005f; }
        else if (id == ID_R_AMB) { st.ambVolume = p; vf::setAmbientVolume(p); }
        else if (id == ID_R_CYC_FOCUS) {
            int v = (int)(5 + p * 175 + 0.5f);
            st.focusCyc = max(5, min(180, (v / 5) * 5));          // 5 分钟一档
            if (rt.running && rt.phase == 0) rt.phaseEnd = rt.phaseStart + phaseTotalMs();
        }
        else if (id == ID_R_CYC_BREAK) {
            st.breakCyc = max(1, min(60, (int)(1 + p * 59 + 0.5f)));
            if (rt.running && rt.phase == 1) rt.phaseEnd = rt.phaseStart + phaseTotalMs();
        }
        if ((id == ID_R_MIN || id == ID_R_MAX || id == ID_R_FIXED) && rt.running && rt.phase == 0)
            scheduleNext(nowMs());
        rt.dirty = true;
        break;
    }
}

// ============================================================
// 托盘
// ============================================================
#define WM_TRAYICON (WM_APP + 1)
#define IDM_SHOW   9001
#define IDM_PAUSE  9002
#define IDM_BELL   9003
#define IDM_QUIT   9004

static void setAutoStart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            wstring v = L"\"" + wstring(path) + L"\"";
            RegSetValueExW(k, L"NudgeBell", 0, REG_SZ, (const BYTE*)v.c_str(),
                           (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(k, L"NudgeBell");
        }
        RegCloseKey(k);
    }
}

static void trayAdd() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"NudgeBell · 待命");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void trayTip(const wstring& s) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, s.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void showWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void trayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"显示主窗口");
    AppendMenuW(m, MF_STRING, IDM_PAUSE, rt.running ? L"暂停专注" : L"继续专注");
    AppendMenuW(m, MF_STRING, IDM_BELL, L"立刻来一声提示音");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

// ============================================================
// 交互
// ============================================================
static void onCommand(int id) {
    if (id == ID_BLIND || id == ID_FIXED) {
        st.mode = (id == ID_BLIND) ? 0 : 1;
        if (rt.running) scheduleNext(nowMs());
        rt.scrollY = 0;
        rt.dirty = true;
        toast(st.mode == 0 ? L"盲盒模式 · 时间未知才有意思" : L"定时模式 · 到点必响");
        return;
    }
    if (id >= ID_STEP_M && id < ID_STEP_M + 200) { nudgeSlider(id - ID_STEP_M, -1); return; }
    if (id >= ID_STEP_P && id < ID_STEP_P + 200) { nudgeSlider(id - ID_STEP_P, +1); return; }
    if (id == ID_SAVE) {
        saveState(true);
        rt.dirty = false;
        toast(L"设置已保存");
        return;
    }
    if (id == ID_PREV_MONTH) { if (rt.viewMonth > -11) rt.viewMonth--; return; }
    if (id == ID_NEXT_MONTH) { if (rt.viewMonth < 0) rt.viewMonth++; return; }
    if (id == ID_GEAR) { rt.page = 1; rt.scrollY = 0; buildSettings(); return; }
    if (id == ID_SHEET_CLOSE) {
        if (rt.dirty) { saveState(true); rt.dirty = false; }   // 关面板兜底，别让改动丢了
        rt.page = 0;
        return;
    }
    if (id == ID_MIN) { ShowWindow(g_hwnd, SW_MINIMIZE); return; }
    if (id == ID_CLOSE) {
        ShowWindow(g_hwnd, SW_HIDE);
        if (!rt.hideTipShown) {
            rt.hideTipShown = true;
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = g_hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_INFO;
            wcscpy_s(nid.szInfoTitle, L"NudgeBell 仍在运行");
            wcscpy_s(nid.szInfo, L"已收进右下角托盘，盲盒会继续计时。点托盘图标唤回窗口。");
            nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        return;
    }
    if (id == ID_MAIN) {
        if (st.sessionStart == 0) startSession();
        else if (rt.running && st.cycleOn && rt.phase == 1) endBreak(nowMs(), true);
        else if (rt.running) pauseSession();
        else resumeSession();
        return;
    }
    if (id == ID_SW_CYCLE) {
        st.cycleOn = !st.cycleOn;
        if (st.cycleOn) {
            rt.phase = 0;
            rt.phaseStart = nowMs();
            rt.phaseEnd = rt.phaseStart + phaseTotalMs();
            if (rt.running) scheduleNext(nowMs());
            toast(L"专注 " + to_wstring(st.focusCyc) + L" 分后提醒休息 " + to_wstring(st.breakCyc) + L" 分");
        } else {
            rt.phase = 0;
            if (rt.running) scheduleNext(nowMs());
            toast(L"已关闭专注周期");
        }
        rt.dirty = true;
        return;
    }
    if (id == ID_BELL) { doRing(true); return; }
    if (id == ID_MUTE) {
        st.soundOn = !st.soundOn;
        if (st.soundOn) st.volume = max(0.15f, st.volume);
        rt.dirty = true;
        toast(st.soundOn ? L"音效已开" : L"音效已静音");
        return;
    }
    if (id == ID_SW_CARD) { st.cardOn = !st.cardOn; rt.dirty = true; return; }
    if (id == ID_SW_NOTIFY) { st.notifyOn = !st.notifyOn; rt.dirty = true; return; }
    if (id == ID_SW_TOPMOST) {
        st.topmost = !st.topmost;
        SetWindowPos(g_hwnd, st.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
        rt.dirty = true;
        toast(st.topmost ? L"窗口已置顶" : L"已取消置顶");
        return;
    }
    if (id == ID_SW_FRONT) {
        st.bringFront = !st.bringFront;
        saveState();
        toast(st.bringFront ? L"响铃时会自动跳到前面" : L"响铃时保持安静");
        return;
    }
    if (id == ID_SW_AUTO) {
        st.autoStart = !st.autoStart;
        setAutoStart(st.autoStart);
        rt.dirty = true;
        toast(st.autoStart ? L"已设为开机启动" : L"已取消开机启动");
        return;
    }
    if (id == ID_RESET) { resetToday(); return; }
    if (id == ID_USERDATA) {
        wstring p = statePath();
        wstring dir = p.substr(0, p.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id >= ID_SOUND_BASE && id < ID_SOUND_BASE + 12) {
        int i = id - ID_SOUND_BASE;
        bool willEnable = !st.enabled[i];
        st.enabled[i] = willEnable;
        if (willEnable) {
            static Buf cache[12];
            static bool loaded[12] = {false};
            if (!loaded[i]) { cache[i] = vf::makeSound(i); loaded[i] = true; }
            if (st.soundOn) vf::playOnce(cache[i], st.volume);
            toast(wstring(vf::soundName(i)) + L" · " + vf::soundTag(i), 1800);
        } else {
            int cnt = 0;
            for (int k = 0; k < 12; k++) if (st.enabled[k]) cnt++;
            if (cnt == 0) {
                st.enabled[i] = true;
                toast(L"至少保留一个提示音");
                return;
            }
            toast(wstring(vf::soundName(i)) + L" 已关闭");
        }
        rt.dirty = true;
        return;
    }
    if (id >= ID_AMB_BASE && id < ID_AMB_BASE + 4) {
        st.ambient = id - ID_AMB_BASE;
        if (st.ambient == 0) vf::stopAmbient();
        else vf::startAmbient(st.ambient, st.ambVolume);
        rt.dirty = true;
        return;
    }
}

// ============================================================
// 主循环 tick
// ============================================================
static long long g_lastTick = 0;
static long long g_lastSave = 0;
static long long g_lastDelCheck = 0;
static unsigned g_frame = 0;

static void tick() {
    long long now = nowMs();
    long long dt = g_lastTick ? now - g_lastTick : 33;
    if (dt > 5000) dt = 5000;
    g_lastTick = now;

    // 每 0.8 秒看一眼数据文件还在不在。用户删档要当场生效，不能等 15 秒的存盘周期。
    if (now - g_lastDelCheck > 800) { resetStateIfFileDeleted(); g_lastDelCheck = now; }

    ensureToday();

    if (rt.running) {
        if (st.cycleOn && rt.phase == 1) {
            // 休息段：不计入专注时长，也不响盲盒
            if (now >= rt.phaseEnd) endBreak(now, false);
        } else {
            st.focusMs += dt;
            if (now >= rt.nextBellAt) {
                doRing(false);
                scheduleNext(now);
            }
            if (st.cycleOn && rt.phaseEnd > 0 && now >= rt.phaseEnd) startBreak(now);
        }
    }

    if (rt.cardVisible) {
        if (now > rt.cardUntil) {
            rt.cardVisible = false;
            hideCardPopup();
        } else if (now - g_cardPaintAt > 80) {
            g_cardPaintAt = now;
            paintCardWindow();          // 只重画那张小卡片，不动主窗口
        }
    }
    if (!rt.toastText.empty() && now > rt.toastUntil) rt.toastText.clear();

    if (now - g_lastSave > 15000) {
        resetStateIfFileDeleted();      // 档被用户删了就先归零，别再把旧数据写回去
        saveState(false);
        g_lastSave = now;
    }

    // 托盘提示
    int sec = (int)(now / 1000);
    if (sec != rt.trayLastSec) {
        rt.trayLastSec = sec;
        if (rt.running && st.cycleOn && rt.phase == 1) {
            trayTip(L"NudgeBell · 休息中 " + fmtMMSS(max(0LL, rt.phaseEnd - now)));
        } else if (rt.running) {
            long long left = rt.nextBellAt - now;
            wstring tip = L"NudgeBell · 已专注 " + fmtClock(st.focusMs) +
                          L" · 下次约 " + to_wstring(max(0LL, left / 60000)) + L" 分内";
            if (st.cycleOn && rt.phaseEnd > now)
                tip += L" · 本轮剩 " + to_wstring(max(0LL, (rt.phaseEnd - now) / 60000)) + L" 分";
            trayTip(tip);
        } else if (st.sessionStart) trayTip(L"NudgeBell · 已暂停");
        else trayTip(L"NudgeBell · 待命");
    }
}

// ============================================================
// 窗口过程
// ============================================================
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        UINT_PTR tid = SetTimer(h, 1, 33, nullptr);
        if (!tid) {
            FILE* f = nullptr;
            fopen_s(&f, "timer_error.txt", "w");
            if (f) { fprintf(f, "SetTimer failed: %lu\n", GetLastError()); fclose(f); }
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_TIMER: {
        g_timerN++;
        tick();
        g_frame++;
        bool busy = rt.running || rt.cardVisible || !rt.toastText.empty();
        if (busy || (g_frame % 6) == 0) {
            InvalidateRect(h, nullptr, FALSE);
            UpdateWindow(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(l) / S, y = GET_Y_LPARAM(l) / S;
        if (rt.activeSlider > 0) {
            setSliderValue(rt.activeSlider, x);
            InvalidateRect(h, nullptr, FALSE);
        } else {
            int id = hitTest(x, y);
            if (id != rt.hover) {
                rt.hover = id;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(l) / S, y = GET_Y_LPARAM(l) / S;
        int id = hitTest(x, y);
        SetCapture(h);
        rt.pressed = id;
        if (id == ID_R_MIN || id == ID_R_MAX || id == ID_R_FIXED || id == ID_R_VOL ||
            id == ID_R_AMB || id == ID_R_CYC_FOCUS || id == ID_R_CYC_BREAK) {
            rt.activeSlider = id;
            setSliderValue(id, x);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (rt.activeSlider > 0) {
            rt.activeSlider = -1;
            rt.dirty = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        float x = GET_X_LPARAM(l) / S, y = GET_Y_LPARAM(l) / S;
        int id = hitTest(x, y);
        if (id == rt.pressed && id > 0) {
            onCommand(id);
            InvalidateRect(h, nullptr, FALSE);
        }
        rt.pressed = -1;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (rt.page == 1) {
            rt.scrollY -= delta / 120.0f * 48.0f;
            rt.scrollY = max(0.0f, min(rt.scrollMax, rt.scrollY));
            InvalidateRect(h, nullptr, FALSE);
        } else {
            rt.mainY -= delta / 120.0f * 56.0f;
            rt.mainY = max(0.0f, min(rt.mainMax, rt.mainY));
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &p);
        float x = p.x / S, y = p.y / S;
        if (rt.page == 0 && y < TITLE_H) {
            if (!rModeSeg().Contains(x, y) && !rGear().Contains(x, y) &&
                !rMin().Contains(x, y) && !rClose().Contains(x, y))
                return HTCAPTION;
        } else if (rt.page == 1 && y < 56 && x < CW - 56) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_NCCALCSIZE:
        if (w) return 0;
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mi = (MINMAXINFO*)l;
        mi->ptMinTrackSize.x = mi->ptMaxTrackSize.x = (LONG)(CW * S);
        mi->ptMinTrackSize.y = mi->ptMaxTrackSize.y = (LONG)(CH * S);
        return 0;
    }
    case WM_KEYDOWN: {
        if (w == VK_ESCAPE) {
            if (rt.cardVisible) { rt.cardVisible = false; hideCardPopup(); }
            else if (rt.page == 1) rt.page = 0;
            InvalidateRect(h, nullptr, FALSE);
        } else if (w == VK_SPACE && rt.page == 0) {
            onCommand(ID_MAIN);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        if (!rt.hideTipShown) {
            rt.hideTipShown = true;
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = h;
            nid.uID = 1;
            nid.uFlags = NIF_INFO;
            wcscpy_s(nid.szInfoTitle, L"NudgeBell 仍在运行");
            wcscpy_s(nid.szInfo, L"已收进右下角托盘，盲盒会继续计时。");
            nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(l) == WM_LBUTTONUP) {
            if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
            else showWindow();
        } else if (LOWORD(l) == WM_RBUTTONUP) {
            trayMenu();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDM_SHOW) showWindow();
        else if (id == IDM_PAUSE) { onCommand(ID_MAIN); InvalidateRect(h, nullptr, FALSE); }
        else if (id == IDM_BELL) { doRing(false); InvalidateRect(h, nullptr, FALSE); }
        else if (id == IDM_QUIT) {
            markPausedOnExit();
            vf::stopAmbient();
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = h;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
        }
        return 0;
    }
    case WM_ENDSESSION:
        if (w) markPausedOnExit();     // 关机、注销也按"退出"算
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, w, l);
}

// ============================================================
// 自检
// ============================================================
static int selfTest() {
    vector<string> log;
    int pass = 0, total = 0;
    auto check = [&](const char* name, bool ok, const char* extra = "") {
        total++;
        if (ok) pass++;
        char line[256];
        sprintf(line, "%s  %s %s", ok ? "PASS" : "FAIL", name, extra);
        log.push_back(line);
    };

    // 音频合成
    bool allOk = true;
    for (int i = 0; i < 12; i++) {
        Buf b = vf::makeSound(i);
        float mx = 0;
        for (float v : b) mx = max(mx, fabsf(v));
        if (b.size() < 1000 || mx < 0.1f || mx > 1.01f) {
            allOk = false;
            char tb[160];
            sprintf(tb, "  sound %d bad size=%d peak=%.3f", i, (int)b.size(), mx);
            log.push_back(tb);
        }
    }
    check("12 种音色合成正常", allOk);

    Buf amb = vf::makeAmbient(1);
    check("环境音生成正常", amb.size() > 10000);

    // 盲盒：区间、分布、随机性
    st.mode = 0; st.minGap = 4; st.maxGap = 12;
    {
        int N = 2000;
        long long mn = 1LL << 40, mx = 0;
        double sum = 0, sum2 = 0;
        int bucket[4] = {0, 0, 0, 0};
        bool inRange = true;
        long long prev = -1;
        int sameAsPrev = 0;
        for (int i = 0; i < N; i++) {
            long long ms = nextGapMs();
            double m = ms / 60000.0;
            if (m < st.minGap - 0.001 || m > st.maxGap + 0.001) inRange = false;
            mn = min(mn, ms); mx = max(mx, ms);
            sum += m; sum2 += m * m;
            int b = (int)((m - st.minGap) / (st.maxGap - st.minGap) * 4);
            if (b > 3) b = 3; if (b < 0) b = 0;
            bucket[b]++;
            if (prev >= 0 && ms == prev) sameAsPrev++;
            prev = ms;
        }
        double mean = sum / N;
        double var = sum2 / N - mean * mean;
        char b1[256];
        sprintf(b1, "盲盒2000次: 范围 %.2f~%.2f 分, 均值 %.2f, 标准差 %.2f",
                mn / 60000.0, mx / 60000.0, mean, sqrt(max(0.0, var)));
        log.push_back(b1);
        sprintf(b1, "  四等分区间分布: %d / %d / %d / %d", bucket[0], bucket[1], bucket[2], bucket[3]);
        log.push_back(b1);
        sprintf(b1, "  相邻两次完全相同: %d 次", sameAsPrev);
        log.push_back(b1);
        check("盲盒间隔全部落在 4~12 分内", inRange);
        check("盲盒确实在变化（非固定值）", sameAsPrev < N / 10);
        check("盲盒覆盖整个区间", bucket[0] > 20 && bucket[3] > 20);
    }

    // 定时：每次都必须精确等于设定值
    {
        st.mode = 1; st.fixedGap = 15;
        bool exact = true;
        for (int i = 0; i < 200; i++) {
            if (nextGapMs() != 15LL * 60000LL) exact = false;
        }
        check("定时模式精确等于设定间隔", exact);

        st.fixedGap = 20;
        exact = true;
        for (int i = 0; i < 50; i++) if (nextGapMs() != 20LL * 60000LL) exact = false;
        check("改设置后定时立即生效", exact);
        st.mode = 0;
    }

    // 日期
    wstring k = todayKey();
    check("日期格式正确", k.size() == 10 && k[4] == L'-' && k[7] == L'-');

    // 时长格式
    check("时长格式化", fmtClock(2742000) == L"45:42");
    check("补零格式", fmtClock(5000) == L"00:05");

    // 状态存取
    st.minGap = 7; st.maxGap = 21; st.volume = 0.42f; st.bells = 3;
    saveState();
    st.minGap = 0; st.maxGap = 0; st.volume = 0; st.bells = 0;
    loadState();
    check("设置可持久化", st.minGap == 7 && st.maxGap == 21 && fabsf(st.volume - 0.42f) < 0.02f);
    check("计数可持久化", st.bells == 3);

    {
        char sum[64];
        sprintf(sum, "", pass, total);
        sprintf(sum, "\n%d/%d passed", pass, total);
        log.push_back(sum);
        FILE* f = nullptr;
        fopen_s(&f, "selftest.txt", "w");
        if (f) {
            for (auto& l : log) fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
    }
    if (g_testMode) DeleteFileW(statePath().c_str());
    return pass == total ? 0 : 1;
}

// ============================================================
// 入口
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    srand((unsigned)time(nullptr));
    timeBeginPeriod(1);   // 提高 WM_TIMER 精度，避免刷新率被系统拉低

    // DPI：显式声明，避免系统虚拟化二次缩放
    SetProcessDPIAware();
    {
        HDC screen = GetDC(nullptr);
        g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        S = g_dpi / 96.0f;
    }

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiToken, &gsi, nullptr);

    loadState();
    ensureToday();
    // 启动时本来就没有数据文件（全新用户/刚删过），不要弹出"已重置"的提示
    if (stateFileGone()) g_stateResetDone = true;
    initFonts();

    if (wcsstr(GetCommandLineW(), L"--shot")) {
        // 导出界面预览（mock 数据）
        st.todayDate = todayKey();
        st.focusMs = 2742000;
        st.bells = 3;
        st.cardOn = true;
        st.cycleOn = true;
        st.focusCyc = 90;
        st.breakCyc = 20;
        st.minGap = 3;
        st.maxGap = 6;
        // 造数据：上月整月 + 本月至今，覆盖全部五档色阶
        time_t tt = time(nullptr);
        tm tmv;
        localtime_s(&tmv, &tt);
        auto mockMonth = [&](int y, int m0, int maxDay) {
            static const int DD[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int dn = DD[m0];
            if (m0 == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) dn = 29;
            if (maxDay > 0 && maxDay < dn) dn = maxDay;
            for (int d = 1; d <= dn; d++) {
                wchar_t buf[32];
                swprintf_s(buf, L"%04d-%02d-%02d", y, m0 + 1, d);
                // 按日期哈希取值：确定性（每次截图一致）但分布散开，像真实的一个月
                unsigned hs = (unsigned)d * 2654435761u + (unsigned)m0 * 7919u + 12345u;
                hs ^= hs >> 13; hs *= 2246822519u; hs ^= hs >> 15;
                int t = (int)(hs % 10);
                long long v;
                if (t < 2)          v = 0;                              // 没学
                else if (t < 4)     v = (6 + hs % 9) * 60000LL;         // < 15 分
                else if (t < 6)     v = (16 + hs % 13) * 60000LL;       // < 30 分
                else if (t < 8)     v = (32 + hs % 26) * 60000LL;       // < 1 小时
                else                v = (62 + hs % 70) * 60000LL;       // >= 1 小时

                st.hist.push_back({ buf, v });
            }
        };
        for (int k = 3; k >= 1; k--) {                 // 前 3 个月都铺满，翻月才有东西看
            int py = tmv.tm_year + 1900, pm = tmv.tm_mon - k;
            while (pm < 0) { pm += 12; py -= 1; }
            mockMonth(py, pm, 0);
        }
        mockMonth(tmv.tm_year + 1900, tmv.tm_mon, tmv.tm_mday - 1);   // 本月至今
        st.sessionStart = nowMs() - 2742000;
        rt.running = true;
        rt.lastBellAt = nowMs() - 220000;
        rt.nextBellAt = nowMs() + 160000;

        exportPng(L"preview.png", 480);

        rt.viewMonth = -1; // 上个月：整月数据，色阶完整
        rt.mainY = 9999;   // 每张都要重设，否则只会被夹到上一张的上限
        exportPng(L"preview-heat.png", 480);

        rt.viewMonth = 0;  // 本月：验证今天的高亮圈与"本月"标题
        rt.mainY = 9999;
        exportPng(L"preview-heat-this.png", 480);

        rt.viewMonth = -3; // 6 行月份：验证卡片变高后排版不错位
        rt.mainY = 9999;
        exportPng(L"preview-heat-6rows.png", 480);

        rt.mainY = 0;
        rt.viewMonth = 0;

        // 暂停态：主按钮是渐变填充，用于检查渐变是否有接缝
        rt.running = false;
        st.paused = true;
        exportPng(L"preview-paused.png", 480);
        rt.running = true;
        st.paused = false;

        // 休息态：圆环转绿、中心显示休息倒计时、按钮变"跳过休息"
        rt.running = true;
        st.paused = false;
        st.cycleOn = true;
        rt.phase = 1;
        rt.phaseStart = nowMs() - 8 * 60000;
        rt.phaseEnd = nowMs() + 11 * 60000 + 30000;
        rt.mainY = 0;
        exportPng(L"preview-break.png", 480);
        rt.phase = 0;

        rt.page = 1;
        rt.scrollY = 0;
        rt.dirty = false;
        exportPng(L"preview-settings.png", 480);
        rt.dirty = true;                      // 演示"有改动未保存"
        exportPng(L"preview-settings-dirty.png", 480);
        rt.dirty = false;

        st.mode = 1;                          // 定时模式：只有一根滑杆
        rt.scrollY = 0;
        exportPng(L"preview-settings-fixed.png", 480);
        st.mode = 0;
        rt.scrollY = 620;
        exportPng(L"preview-settings2.png", 480);
        rt.scrollY = 0;
        rt.page = 0;

        rt.cardVisible = true;
        rt.cardKind = L"进度自检";
        rt.cardText = L"把刚冒出来的想法写三个字，别让它跑了。";
        rt.cardSound = L"林间鸟鸣";
        rt.cardFocus = 2742000;
        rt.cardBells = 3;
        rt.cardManual = false;
        rt.cardStartAt = nowMs();
        rt.cardUntil = nowMs() + 4000;
        exportCardPng(L"preview-card.png", 460);

        // 休息提醒卡
        rt.cardKind = L"该休息了";
        rt.cardText = L"专注 90 分钟到。先闭眼 10 秒，再站起来走一走、看看远处，20 分钟后回来。";
        rt.cardSound = vf::soundName(0);
        rt.cardStartAt = nowMs();
        rt.cardUntil = nowMs() + 4000;
        exportCardPng(L"preview-breakcard.png", 460);

        GdiplusShutdown(g_gdiToken);
        return 0;
    }

    if (wcsstr(GetCommandLineW(), L"--selftest")) {
        g_testMode = true;
        int r = selfTest();
        GdiplusShutdown(g_gdiToken);
        return r;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NudgeBell";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, 0);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, 0);
    RegisterClassExW(&wc);

    int w = (int)(CW * S), hh = (int)(CH * S);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - hh) / 2;

    g_hwnd = CreateWindowExW(0, L"NudgeBell", L"NudgeBell",
                             WS_OVERLAPPEDWINDOW, x, y, w, hh,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // 去掉标题栏后重新定位
    SetWindowPos(g_hwnd, nullptr, x, y, w, hh, SWP_FRAMECHANGED | SWP_NOZORDER);

    // Win11 系统圆角（必须在窗口创建完成后设置）
    {
        int pref = 2; // DWMWCP_ROUND
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            typedef HRESULT(WINAPI * PFN)(HWND, DWORD, LPCVOID, DWORD);
            PFN fn = (PFN)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (fn) fn(g_hwnd, 33, &pref, sizeof(pref));
        }
    }

    if (st.topmost) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    trayAdd();
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    restoreSession();

    // 环境音
    if (st.ambient > 0) vf::startAmbient(st.ambient, st.ambVolume);

    if (wcsstr(GetCommandLineW(), L"--capture")) {
        // 真实弹一张卡片，然后把桌面上那块像素抓下来，验证它真的显示出来了
        g_testMode = true;
        st.cardOn = true;
        st.soundOn = false;
        rt.cardKind = L"该休息了";
        rt.cardText = L"专注 90 分钟到。先闭眼 10 秒，再站起来走一走、看看远处，20 分钟后回来。";
        rt.cardSound = vf::soundName(0);
        rt.cardFocus = 2742000;
        rt.cardBells = 3;
        rt.cardManual = false;
        rt.cardStartAt = nowMs();
        rt.cardUntil = rt.cardStartAt + 30000;
        rt.cardVisible = true;
        g_cardShowN++;
        showCardPopup();

        MSG cm;
        DWORD c0 = GetTickCount();
        while (GetTickCount() - c0 < 900) {
            while (PeekMessageW(&cm, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&cm);
                DispatchMessageW(&cm);
            }
            Sleep(10);
        }
        RECT cr = {};
        if (g_cardHwnd) GetWindowRect(g_cardHwnd, &cr);
        captureRect(cr, L"preview-card-onscreen.png");

        FILE* cf = nullptr;
        fopen_s(&cf, "capture.txt", "w");
        if (cf) {
            fprintf(cf, "hwnd=%p visible=%d rect=(%ld,%ld)-(%ld,%ld)\n", (void*)g_cardHwnd,
                    g_cardHwnd ? (int)IsWindowVisible(g_cardHwnd) : -1,
                    cr.left, cr.top, cr.right, cr.bottom);
            fclose(cf);
        }
        hideCardPopup();
        DestroyWindow(g_hwnd);
        GdiplusShutdown(g_gdiToken);
        return 0;
    }

    if (wcsstr(GetCommandLineW(), L"--autotest")) {
        g_testMode = true;
        vector<string> log;
        int pass = 0, total = 0;
        auto chk = [&](const char* name, bool ok) {
            total++;
            if (ok) pass++;
            char b[200];
            sprintf(b, "%s  %s", ok ? "PASS" : "FAIL", name);
            log.push_back(b);
        };
        auto pump = [&](int ms) {
            MSG mm;
            DWORD t0 = GetTickCount();
            while (GetTickCount() - t0 < (DWORD)ms) {
                while (PeekMessageW(&mm, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&mm);
                    DispatchMessageW(&mm);
                }
                Sleep(5);
            }
        };

        // 从干净状态开始，避免上次测试残留的会话影响判断
        // （真实实例每 15 秒回写一次 state.ini，所以设置也必须钉死，否则会串味）
        st.sessionStart = 0;
        st.paused = false;
        rt.running = false;
        rt.page = 0;
        st.bells = 0;
        st.focusMs = 0;
        st.todayDate = todayKey();
        st.cardOn = true;
        st.soundOn = true;
        st.cycleOn = true;
        st.focusCyc = 90;
        st.breakCyc = 20;
        st.minGap = 3;
        st.maxGap = 6;
        st.volume = 0.7f;

        pump(400);
        chk("初始未开始", st.sessionStart == 0 && !rt.running);

        // 空闲时（无鼠标事件）是否仍按时重绘
        // 先确保窗口真的可见（否则 Windows 不会发 WM_PAINT）
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        pump(300);
        int f0 = g_paintN, t0n = g_timerN;
        pump(1000);
        {
            char b[96];
            sprintf(b, "空闲1秒: timer %d, paint %d, vis %d", g_timerN - t0n, g_paintN - f0,
                    IsWindowVisible(g_hwnd) ? 1 : 0);
            log.push_back(b);

        }
        int idleFrames = g_paintN - f0;
        {
            char b[96];
            sprintf(b, "空闲1秒重绘 %d 帧", idleFrames);
            chk("空闲时仍在刷新画面", idleFrames >= 3);
            log.push_back(b);
        }

        onCommand(ID_MAIN);
        int f1 = g_paintN;
        pump(1800);
        int runFrames = g_paintN - f1;
        {
            char b[96];
            sprintf(b, "运行1.8秒重绘 %d 帧", runFrames);
            chk("运行时持续刷新（约30fps）", runFrames >= 30);
            log.push_back(b);
        }
        chk("开始后计时运行", rt.running);
        chk("专注时长在累积", st.focusMs > 1200);

        // 到点自动响铃：只设时间，不手动触发，看调度器是否自己响
        {
            int b0 = st.bells;
            int n0 = g_cardShowN;
            rt.cardVisible = false;
            long long t0b = nowMs();
            rt.nextBellAt = t0b + 1500;
            pump(3200);
            char b[128];
            sprintf(b, "设 1.5 秒后响，实测 %.1f 秒后触发", (rt.lastBellAt - t0b) / 1000.0);
            log.push_back(b);
            chk("到点后自动响铃（无需手动）", st.bells == b0 + 1);
            chk("自动响铃弹出卡片", g_cardShowN == n0 + 1);
            rt.cardVisible = false;
        }

        int bellsBefore = st.bells;
        doRing(false);
        chk("响铃后弹出卡片", rt.cardVisible);
        chk("卡片是独立浮窗且已显示", g_cardHwnd && IsWindowVisible(g_cardHwnd));
        chk("浮窗不抢焦点", g_cardHwnd && (GetWindowLongPtrW(g_cardHwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0);
        {
            RECT cr, work;
            GetWindowRect(g_cardHwnd, &cr);
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            bool onScreen = cr.left >= work.left && cr.top >= work.top &&
                            cr.right <= work.right && cr.bottom <= work.bottom;
            int cw = cr.right - cr.left, chh = cr.bottom - cr.top;
            char b[160];
            sprintf(b, "浮窗 %dx%d @ (%d,%d)，工作区 %dx%d", cw, chh, cr.left, cr.top,
                    (int)(work.right - work.left), (int)(work.bottom - work.top));
            log.push_back(b);
            chk("浮窗完整落在屏幕内", onScreen);
            chk("浮窗尺寸合理（宽 380 逻辑）", cw > (int)(300 * S) && cw < (int)(460 * S) &&
                                           chh > (int)(150 * S) && chh < (int)(400 * S));
        }
        chk("唤醒次数 +1", st.bells == bellsBefore + 1);
        chk("卡片有文案", rt.cardText.size() > 5);

        long long before = st.focusMs;
        onCommand(ID_MAIN);
        pump(1200);
        chk("暂停后计时停止", !rt.running && st.focusMs - before < 400);

        onCommand(ID_MAIN);
        pump(300);
        chk("可恢复", rt.running);

        // 提示条能自己消失（曾经因为 float 精度问题挂了很久）
        {
            toast(L"测试提示", 500);
            chk("提示条立即出现", !rt.toastText.empty());
            pump(1300);
            chk("提示条到点自动消失", rt.toastText.empty());
        }

        onCommand(ID_GEAR);
        pump(300);
        chk("设置页可打开", rt.page == 1);
        int itemCount = (int)g_items.size();
        chk("设置项已构建", itemCount > 20);
        onCommand(ID_SHEET_CLOSE);
        pump(200);
        chk("设置页可关闭", rt.page == 0);

        st.mode = 1;
        onCommand(ID_BLIND);
        chk("模式可切回盲盒", st.mode == 0);

        // ---- 专注周期 90 / 20 ----
        chk("默认专注 90 分 / 休息 20 分", st.focusCyc == 90 && st.breakCyc == 20);
        chk("默认开启专注周期", st.cycleOn);
        {
            st.cardOn = true;
            st.sessionStart = 0;
            rt.running = false;
            st.focusMs = 0;
            onCommand(ID_MAIN);
            long long span = rt.phaseEnd - rt.phaseStart;
            char b[128];
            sprintf(b, "专注段实测 %.1f 分", span / 60000.0);
            log.push_back(b);
            chk("开始后进入专注段，长度 90 分", rt.phase == 0 &&
                span > 90 * 60000LL - 2000 && span < 90 * 60000LL + 2000);

            // 把本轮压到 1.5 秒后结束，验证自动切换
            rt.phaseEnd = nowMs() + 1500;
            rt.nextBellAt = nowMs() + 120000;      // 期间不该有盲盒响
            int brk0 = st.breaks;
            pump(3200);
            chk("专注到点自动进入休息", rt.phase == 1);
            chk("休息段长度 = 20 分", rt.phaseEnd - rt.phaseStart == 20 * 60000LL);
            chk("休息次数 +1", st.breaks == brk0 + 1);
            chk("进入休息时弹卡片", rt.cardVisible);
            rt.cardVisible = false;

            // 休息期间既不计专注，也不响盲盒
            long long f1 = st.focusMs;
            int bells1 = st.bells;
            rt.nextBellAt = nowMs() - 1;           // 就算到点也不该响
            pump(1600);
            chk("休息期间不计入专注时长", st.focusMs - f1 < 300);
            chk("休息期间不响盲盒", st.bells == bells1);

            // 休息到点自动回到专注
            rt.phaseEnd = nowMs() + 1200;
            pump(2600);
            chk("休息结束自动回到专注", rt.phase == 0);
            chk("回到专注后重新排了盲盒", rt.nextBellAt > nowMs());

            // 模拟"关掉程序再打开"：内存里的阶段计时全丢，只剩 paused 标记
            {
                st.sessionStart = nowMs() - 3600000;
                rt.running = false;
                st.paused = true;
                rt.phase = 0;
                rt.phaseEnd = 0;
                rt.phaseLeftMs = 0;
                onCommand(ID_MAIN);
                chk("重启后继续专注，本轮计时重新开始",
                    rt.running && rt.phase == 0 && rt.phaseEnd > nowMs() + 89 * 60000LL);
            }

            // 跳过休息
            rt.phase = 1;
            rt.phaseStart = nowMs() - 60000;
            rt.phaseEnd = nowMs() + 600000;
            onCommand(ID_MAIN);
            chk("跳过休息直接回到专注", rt.phase == 0);
        }

        // ---- 真实鼠标路径：点在音量滑杆上，值应该跟着变 ----
        {
            rt.page = 1;
            buildSettings();
            RectF tr; bool found = false;
            for (auto& it : g_items) if (it.id == ID_R_VOL) { tr = it.track; found = true; }
            chk("音量滑杆项存在", found);
            float px = (tr.X + tr.Width * 0.85f) * S;
            float py = (tr.Y + tr.Height / 2) * S;
            int before = (int)(st.volume * 100);
            SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((int)px, (int)py));
            SendMessageW(g_hwnd, WM_LBUTTONUP, 0, MAKELPARAM((int)px, (int)py));
            char b[160];
            sprintf(b, "滑杆 track=(%.0f,%.0f,%.0f) 点=(%.0f,%.0f) hit=%d page=%d scrollY=%.0f S=%.2f",
                    tr.X, tr.Y, tr.Width, px / S, py / S, hitTest(px / S, py / S), rt.page, rt.scrollY, S);
            log.push_back(b);
            sprintf(b, "点滑杆 85%% 处：%d%% -> %d%%", before, (int)(st.volume * 100));
            log.push_back(b);
            chk("点滑杆能把值拖过去", (int)(st.volume * 100) > 60);
            rt.page = 0;
        }

        // ---- 加减按钮 + 保存按钮（真实鼠标路径）----
        {
            rt.page = 1;
            rt.dirty = false;
            buildSettings();

            auto clickAt = [&](float lx, float ly) {
                SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((int)(lx * S), (int)(ly * S)));
                SendMessageW(g_hwnd, WM_LBUTTONUP, 0, MAKELPARAM((int)(lx * S), (int)(ly * S)));
            };
            auto itemRect = [&](int id, RectF& row) {
                for (auto& it : g_items) if (it.id == id) { row = it.r; return true; }
                return false;
            };

            RectF row;
            chk("找到专注滑杆行", itemRect(ID_R_CYC_FOCUS, row));
            int f0 = st.focusCyc;
            // 加号在行的右侧
            clickAt(row.X + row.Width - 117, row.Y + 15);
            char b[160];
            sprintf(b, "点 + ：专注 %d -> %d 分", f0, st.focusCyc);
            log.push_back(b);
            chk("点加号专注 +5 分", st.focusCyc == f0 + 5);
            chk("改完之后标记为未保存", rt.dirty);
            // 减号在行的左侧
            clickAt(row.X + 85, row.Y + 15);
            chk("点减号专注 -5 分", st.focusCyc == f0);

            RectF row2;
            itemRect(ID_R_CYC_BREAK, row2);
            int b0 = st.breakCyc;
            clickAt(row2.X + row2.Width - 117, row2.Y + 15);
            chk("点加号休息 +1 分", st.breakCyc == b0 + 1);
            clickAt(row2.X + 85, row2.Y + 15);
            chk("点减号休息 -1 分", st.breakCyc == b0);

            // 加减按钮的边界与联动
            for (int i = 0; i < 60; i++) nudgeSlider(ID_R_CYC_FOCUS, +1);
            chk("加号点到底停在 180 分", st.focusCyc == 180);
            for (int i = 0; i < 60; i++) nudgeSlider(ID_R_CYC_FOCUS, -1);
            chk("减号点到底停在 5 分", st.focusCyc == 5);
            st.focusCyc = 90;

            st.minGap = 3; st.maxGap = 6;
            for (int i = 0; i < 10; i++) nudgeSlider(ID_R_MIN, +1);   // 3 -> 13
            chk("最短推过最长时最长被顶上去", st.minGap == 13 && st.maxGap > st.minGap);
            for (int i = 0; i < 30; i++) nudgeSlider(ID_R_MAX, -1);
            chk("最长压到最短以下时最短被让开", st.maxGap == 2 && st.minGap < st.maxGap);
            st.minGap = 3; st.maxGap = 6;

            // 保存按钮
            RectF sb = rSaveBtn();
            clickAt(sb.X + sb.Width / 2, sb.Y + sb.Height / 2);
            chk("点保存后不再是未保存状态", !rt.dirty);

            // 关面板时兜底保存
            rt.dirty = true;
            onCommand(ID_SHEET_CLOSE);
            chk("关面板会把未保存的改动落盘", !rt.dirty && rt.page == 0);
        }

        // ---- 退出即暂停：下次打开不接着跑 ----
        {
            // 正在计时的时候退出
            st.sessionStart = nowMs() - 1800000;
            st.paused = false;
            rt.running = true;
            rt.phaseStart = nowMs() - 600000;
            rt.phaseEnd = nowMs() + 4800000;
            saveState(true);
            markPausedOnExit();
            chk("退出后内存里是暂停态", !rt.running && st.paused);

            // 模拟重新打开：从磁盘读回来
            st.paused = false;
            st.sessionStart = 0;
            rt.running = true;
            loadState();
            chk("退出时把 paused=1 写进了配置", st.paused && st.sessionStart > 0);

            // 再模拟一次"启动时恢复了上次会话"
            st.paused = false;          // 假设被强杀，配置里还是 0
            rt.running = true;
            restoreSession();
            chk("重新打开后停在暂停，不会自己跑", !rt.running && st.paused);
            chk("会话记录还在（时长不丢）", st.sessionStart > 0);
        }

        // ---- 滑杆范围 ----
        {
            buildSettings();
            setSliderValue(ID_R_CYC_FOCUS, 99999);
            chk("专注时长上限 180 分", st.focusCyc == 180);
            setSliderValue(ID_R_CYC_FOCUS, -99999);
            chk("专注时长下限 5 分", st.focusCyc == 5);
            setSliderValue(ID_R_CYC_BREAK, 99999);
            chk("休息时长上限 60 分", st.breakCyc == 60);
            setSliderValue(ID_R_CYC_BREAK, -99999);
            chk("休息时长下限 1 分", st.breakCyc == 1);
            st.focusCyc = 90;
            st.breakCyc = 20;
            saveState();
        }

        // ---- 历史记录保留窗口：必须能翻满 12 个月，不能被 40 天截断 ----
        {
            st.hist.clear();
            for (int i = 0; i < 300; i++) {
                wchar_t buf[32];
                swprintf_s(buf, L"%04d-%02d-%02d", 2025, 1 + i / 28, 1 + i % 28);
                st.hist.push_back({ buf, (long long)(i + 1) * 60000LL });
            }
            saveState();
            st.hist.clear();
            loadState();
            char b[128];
            sprintf(b, "存了 300 天，读回 %d 天", (int)st.hist.size());
            log.push_back(b);
            chk("历史能完整存下 300 天", (int)st.hist.size() == 300);
        }

        // ---- 盲盒可以设成"3~6 分钟之间随机" ----
        {
            int m0 = st.minGap, m1 = st.maxGap;
            st.minGap = 3;
            st.maxGap = 6;
            long long lo = 1LL << 60, hi = 0;
            for (int i = 0; i < 3000; i++) {
                long long v = nextGapMs();
                lo = min(lo, v);
                hi = max(hi, v);
            }
            char b[160];
            sprintf(b, "3~6 分区间 3000 次采样：%.2f ~ %.2f 分", lo / 60000.0, hi / 60000.0);
            log.push_back(b);
            chk("3~6 分随机不越界", lo >= 3 * 60000LL && hi <= 6 * 60000LL);
            chk("3~6 分区间两端都取得到", lo < 4 * 60000LL && hi > 5 * 60000LL);
            st.minGap = m0;
            st.maxGap = m1;
        }

        // 热力图翻月：按钮要真的能改变月份，且不能越界
        {
            rt.viewMonth = 0;
            onCommand(ID_PREV_MONTH);
            chk("左箭头退到上个月", rt.viewMonth == -1);
            onCommand(ID_NEXT_MONTH);
            chk("右箭头回到本月", rt.viewMonth == 0);
            for (int i = 0; i < 20; i++) onCommand(ID_PREV_MONTH);
            chk("最多只退 11 个月", rt.viewMonth == -11);
            for (int i = 0; i < 30; i++) onCommand(ID_NEXT_MONTH);
            chk("不能翻到未来月份", rt.viewMonth == 0);

            // 滚动到底后，箭头必须仍然点得到（曾经整页滚动把命中区算错）
            rt.cardVisible = false;              // 卡片层会拦截点击，先收起来
            rt.mainY = rt.mainMax;
            float ax = 18 + 14 + 17;
            float ay = HEAT_TOP + 12 + 14 - rt.mainY;
            int hitPrev = hitTest(ax, ay);
            float bx = 18 + (CW - 36) - 14 - 34 + 17;
            int hitNext = hitTest(bx, ay);
            rt.viewMonth = -1;
            int hitNextOn = hitTest(bx, ay);
            rt.viewMonth = 0;
            rt.mainY = 0;
            chk("滚动后左箭头命中正确", hitPrev == ID_PREV_MONTH);
            chk("滚动后右箭头命中正确", hitNext == ID_NEXT_MONTH || hitNextOn == ID_NEXT_MONTH);

            // 12 个月的行数必须落在 4~6 行，且卡片高度容得下网格+页脚
            bool rowsOk = true;
            for (int m = 0; m >= -11; m--) {
                rt.viewMonth = m;
                int r = heatRows();
                if (r < 4 || r > 6) rowsOk = false;
                if (heatCardH() < r * HEAT_STEP - HEAT_GAP + 190) rowsOk = false;   // 正文 190 高
            }
            rt.viewMonth = 0;
            chk("12 个月的日历行数与卡片高度自洽", rowsOk);
            chk("滚动上限随月历高度变化", rt.mainMax > 0);
        }

        char sum[64];
        sprintf(sum, "\n%d/%d passed", pass, total);
        log.push_back(sum);
        FILE* f = nullptr;
        fopen_s(&f, "autotest.txt", "w");
        if (f) {
            for (auto& l : log) fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
        vf::stopAmbient();
        DestroyWindow(g_hwnd);
        DeleteFileW(statePath().c_str());
        GdiplusShutdown(g_gdiToken);
        return pass == total ? 0 : 1;
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    saveState();
    vf::stopAmbient();
    timeEndPeriod(1);
    GdiplusShutdown(g_gdiToken);
    return 0;
}
