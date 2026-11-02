// Дисплей ST7789. Описание и подключение — в disp.h.
#include "disp.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
void   dispStart() {}
void   dispLoop() {}
String dispJson() { return "{\"on\":false}"; }
String dispSet(int, int, int, int, int, int) { return "дисплея на этой плате нет"; }
#else

#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "state.h"
#include "stats.h"
#include "cfg.h"
#include "net.h"
#include "ota.h"
#include "tg.h"
#include "events.h"
#include "weather.h"
#include "schedule.h"
#include "battlife.h"
#include "inv_codes.h"
#include "disp_font.h"

static const int PIN_SCK = 12, PIN_MOSI = 11, PIN_CS = 10, PIN_DC = 13, PIN_RST = 16, PIN_BL = 14, PIN_BTN = 4;
static const int W = 240, H = 320;
static const uint32_t SPI_HZ = 40000000;
static const int32_t TZ_SEC = 3 * 3600;                  // как везде в прошивке
static const uint8_t NPAGE = 6;
static const uint32_t HOME_MS = 120000;                  // без нажатий — обратно на главный
static const uint32_t WAKE_MS = 60000;                   // погашенный экран кнопка будит на минуту
static const uint32_t ALERT_BRIGHT_MS = 600000;          // тревога: первые 10 мин — полная яркость
static const int NPT = 288;                              // график: 24 ч по 5 минут
static const char* const DOW[] = { "пн", "вт", "ср", "чт", "пт", "сб", "вс" };

static SPIClass  s_spi(FSPI);
static uint16_t* s_fb = nullptr;                         // кадр, байты уже в порядке для SPI
static TaskHandle_t s_task = nullptr;

// Настройки: яркость в %, гасить через sleep минут (0 — никогда), остальное
// 0/1. Меняются со страницы — задача перечитывает их перед кадром.
static volatile uint8_t s_day = 80, s_night = 15, s_flip = 0, s_inv = 1, s_bgr = 0, s_sleep = 0;
static volatile bool s_reinit = false;

// Что показывать: собирает loop(), рисует задача. Данные экранов, кроме
// главного, собираются только пока экран открыт.
struct DEv { char when[12]; char text[64]; char lv; };
struct DData {
    uint8_t  page, bl;                 // экран; яркость, %, уже решённая в loop()
    bool     timeOk;
    time_t   loc;
    // шапка
    int8_t   wifi, inet;               // Wi-Fi: 0 нет, 1 слабый, 2 хороший, 3 точка доступа; интернет: tgOnline()
    bool     night;                    // идёт ночная зарядка
    // главный
    bool     invOk, gridOn, socKnown;
    uint8_t  mode, prioOut, prioChg, fault;
    int16_t  tempC;
    float    gridV, gridW, outW, pvW, battV, battI, soc;
    float    pvWh, gridWh, loadWh;
    uint32_t etaS, gridLostS;          // хватит / до полного, с; сколько нет сети, с
    char     next[64];                 // что сделает автоматика
    // график
    bool     gOk;
    int16_t  gPv[NPT], gLoad[NPT];
    uint8_t  gSoc[NPT];
    // неделя
    bool     wOk[7];
    float    wPv[7], wLoad[7], wGrid[7];
    uint8_t  wDow[7];
    // итоги: вчера, 7 дней, месяц
    bool     tOk[3];
    float    tPv[3], tLoad[3], tGrid[3];
    uint16_t tOut[3];
    uint32_t tOutS[3];
    float    price, fc0, fc1;
    // батарея
    bool     bOk;
    int16_t  tMin, tMax;
    float    chgLimA, cvV, socBms, capAh, ahOut, capEst;
    uint8_t  cells;
    uint16_t cMin, cMax, bFault, bWarn;
    uint16_t cellMv[MAX_CELLS];
    // модуль
    char     ip[16], fw[48];
    bool     ap;
    int8_t   rssi;
    uint32_t upS, heapMin;
    uint8_t  nEv;
    DEv      ev[4];
};
static DData s_d;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_have = false;

static uint8_t  s_page = 0;
static uint32_t s_lastPress = 0, s_heavyAt = 0, s_wakeUntil = 0, s_alertAt = 0, s_alertOffAt = 0, s_gridLostAt = 0;
static volatile uint8_t s_blNow = 255;

// ------------------------------------------------------------ провод
static void cmd(uint8_t c, const uint8_t* d = nullptr, size_t n = 0) {
    s_spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    s_spi.write(c);
    if (n) { digitalWrite(PIN_DC, HIGH); s_spi.writeBytes(d, n); }
    digitalWrite(PIN_CS, HIGH);
    s_spi.endTransaction();
}

static void panelInit() {
    digitalWrite(PIN_RST, HIGH); delay(5);
    digitalWrite(PIN_RST, LOW);  delay(20);
    digitalWrite(PIN_RST, HIGH); delay(150);
    cmd(0x01); delay(150);                               // программный сброс
    cmd(0x11); delay(120);                               // выйти из сна
    uint8_t c = 0x55; cmd(0x3A, &c, 1);                  // 16 бит на точку
    // Портрет; переворот — зеркало по обеим осям (MX|MY), BGR — бит 3.
    uint8_t m = (s_flip ? 0xC0 : 0x00) | (s_bgr ? 0x08 : 0x00);
    cmd(0x36, &m, 1);
    cmd(s_inv ? 0x21 : 0x20);                            // у IPS-панелей цвета инвертированы
    cmd(0x13);
    cmd(0x29); delay(10);                                // включить
}

static void push() {
    static const uint8_t ca[4] = { 0, 0, (W - 1) >> 8, (W - 1) & 0xFF };
    static const uint8_t ra[4] = { 0, 0, (H - 1) >> 8, (H - 1) & 0xFF };
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    s_spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    s_spi.write(0x2C);
    digitalWrite(PIN_DC, HIGH);
    const uint8_t* p = (const uint8_t*)s_fb;
    const size_t total = W * H * 2, chunk = 4096;
    for (size_t o = 0; o < total; o += chunk) s_spi.writeBytes(p + o, total - o < chunk ? total - o : chunk);
    digitalWrite(PIN_CS, HIGH);
    s_spi.endTransaction();
}

// ------------------------------------------------------------ рисование
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t C_BG = rgb(12, 15, 21), C_LINE = rgb(38, 45, 60), C_TEXT = rgb(235, 238, 245),
                      C_DIM = rgb(130, 140, 160), C_GREEN = rgb(52, 199, 89), C_AMBER = rgb(255, 176, 32),
                      C_RED = rgb(255, 77, 79), C_SUN = rgb(255, 204, 51), C_BLUE = rgb(77, 163, 255),
                      C_SUNFILL = rgb(92, 76, 22), C_ALERT = rgb(200, 30, 40), C_WHITE = rgb(255, 255, 255);

static inline uint16_t sw(uint16_t c) { return (c >> 8) | (c << 8); }

static void fill(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t v = sw(c);
    for (int j = y; j < y + h; j++) {
        uint16_t* r = s_fb + j * W + x;
        for (int i = 0; i < w; i++) r[i] = v;
    }
}

static void blend(int x, int y, uint16_t c, uint8_t a) {   // a — 0…15
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || !a) return;
    uint16_t* p = s_fb + y * W + x;
    if (a == 15) { *p = sw(c); return; }
    uint16_t b = sw(*p);
    int r = ((c >> 11) * a + (b >> 11) * (15 - a)) / 15;
    int g = (((c >> 5) & 63) * a + ((b >> 5) & 63) * (15 - a)) / 15;
    int bl = ((c & 31) * a + (b & 31) * (15 - a)) / 15;
    *p = sw((r << 11) | (g << 5) | bl);
}

// Линия толщиной 2 точки — для графика и значков.
static void line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        fill(x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

// UTF-8 → код символа; продвигает s.
static uint16_t utf8(const char*& s) {
    uint8_t c = *s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && *s) return ((c & 0x1F) << 6) | (*s++ & 0x3F);
    if ((c & 0xF0) == 0xE0 && s[0] && s[1]) { uint16_t v = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; return v; }
    while ((*s & 0xC0) == 0x80) s++;
    return '?';
}

static const DGlyph* glyph(const DFont& f, uint16_t cp) {
    int lo = 0, hi = f.n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f.g[m].cp == cp) return &f.g[m];
        if (f.g[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return cp == '?' ? nullptr : glyph(f, '?');
}

static int textW(const DFont& f, const char* s) {
    int w = 0;
    while (*s) { const DGlyph* g = glyph(f, utf8(s)); if (g) w += g->adv; }
    return w;
}

// y — базовая линия; maxW > 0 — не шире, лишнее заменяется «…». Возвращает x после текста.
static int text(const DFont& f, int x, int y, const char* s, uint16_t c, int maxW = 0) {
    int x0 = x, ell = maxW > 0 && textW(f, s) > maxW ? textW(f, "…") : 0;
    while (*s) {
        const DGlyph* g = glyph(f, utf8(s));
        if (!g) continue;
        if (ell && x + g->adv > x0 + maxW - ell) { s = "…"; ell = 0; continue; }
        const uint8_t* b = f.bmp + g->off;
        int stride = (g->w + 1) / 2;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                uint8_t v = b[j * stride + i / 2];
                blend(x + g->x + i, y + g->y + j, c, i & 1 ? v & 15 : v >> 4);
            }
        x += g->adv;
    }
    return x;
}
static void textR(const DFont& f, int xr, int y, const char* s, uint16_t c) { text(f, xr - textW(f, s), y, s, c); }

static String dec(float v, int d) { String r = String(v, (unsigned)d); r.replace('.', ','); return r; }
// 1234 → «1,23 кВт», 850 → «850 Вт»
static String watts(float w) {
    float a = fabsf(w);
    return a >= 1000 ? dec(a / 1000, a >= 10000 ? 1 : 2) + " кВт" : String((int)lroundf(a)) + " Вт";
}
static String kwhv(float wh) { return dec(wh / 1000, wh >= 100000 ? 0 : 1); }
static String kwh(float wh) { return kwhv(wh) + " кВт·ч"; }
// 19200 → «5 ч 20 мин», 600 → «10 мин», больше двух суток — «> 2 сут»
static String span(uint32_t s) {
    uint32_t m = s / 60;
    if (m >= 2880) return "> 2 сут";
    if (m < 60) return String(m) + " мин";
    return String(m / 60) + " ч" + (m % 60 ? " " + String(m % 60) + " мин" : String(""));
}
static String hmm(uint32_t s) { char b[12]; snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60)); return b; }

// Строка с подписью слева и значением справа; sub — вторая строка подписи.
static void row(int y, const char* label, const String& value, uint16_t vc, const String& sub) {
    text(FONT_S, 10, y + 17, label, C_DIM);
    if (sub.length()) text(FONT_S, 10, y + 36, sub.c_str(), C_DIM, W - 20);
    textR(FONT_M, W - 10, y + 20, value.c_str(), vc);
    fill(10, y + 42, W - 20, 1, C_LINE);
}
// Короткая строка — для батареи и модуля.
static void row2(int y, const char* label, const String& value, uint16_t vc) {
    text(FONT_S, 10, y + 21, label, C_DIM);
    // Не влезает рядом с подписью — значение мельче.
    bool big = textW(FONT_S, label) + 10 + textW(FONT_M, value.c_str()) <= W - 20;
    textR(big ? FONT_M : FONT_S, W - 10, y + (big ? 22 : 21), value.c_str(), vc);
    fill(10, y + 31, W - 20, 1, C_LINE);
}

// Значки шапки. x — левый край, y — низ.
static void icoWifi(int x, int y, uint16_t c, uint16_t cross) {
    int cx = x + 8, cy = y;
    for (int dy = -14; dy <= 0; dy++)
        for (int dx = -9; dx <= 9; dx++) {
            if (abs(dx) > -dy + 1) continue;                    // клин вверх, 90°
            float r = sqrtf(dx * dx + dy * dy);
            if (r < 2.2f || (r > 5.5f && r < 7.8f) || (r > 10.5f && r < 12.8f)) fill(cx + dx, cy + dy, 1, 1, c);
        }
    if (cross) line(x + 1, y - 13, x + 15, y, cross);
}
static void icoMoon(int x, int y, uint16_t c) {
    for (int dy = -7; dy <= 7; dy++)
        for (int dx = -7; dx <= 7; dx++)
            if (dx * dx + dy * dy <= 42 && (dx - 4) * (dx - 4) + (dy + 3) * (dy + 3) > 30) fill(x + 7 + dx, y - 7 + dy, 1, 1, c);
}
// Значки: Wi-Fi и интернет, ночная зарядка. Возвращает ширину.
static int icons(const DData& d, int x, int y, bool draw) {
    int w = 0;
    if (d.wifi != 3) {
        if (draw) {
            uint16_t c = d.wifi == 0 ? C_RED : d.wifi == 1 ? C_AMBER : C_GREEN;
            icoWifi(x + w, y, c, d.wifi == 0 ? C_RED : d.inet == 0 ? C_AMBER : 0);
        }
        w += 22;
    }
    if (d.night) { if (draw) icoMoon(x + w, y, C_BLUE); w += 18; }
    return w;
}

static void header(const DData& d, const char* title) {
    static const char* const MON[] = { "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек" };
    // Тревога на главном: шапка красная — нет сети или авария.
    bool red = !title && d.invOk && (!d.gridOn || d.fault);
    if (red) fill(0, 0, W, 37, C_ALERT);
    uint16_t tc = red ? C_WHITE : C_TEXT, dc = red ? C_WHITE : C_DIM;
    int x = 10;
    if (d.timeOk) {
        struct tm t;
        gmtime_r(&d.loc, &t);
        char b[24];
        snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
        x = text(FONT_M, 10, 26, b, tc) + 8;
    }
    String right;
    if (title) { char n[8]; snprintf(n, sizeof(n), "%u/%u", d.page + 1, NPAGE); right = n; }
    else if (red && !d.gridOn) right = "сети нет " + hmm(d.gridLostS);
    else if (red) right = "авария " + String(d.fault);
    // «сеть есть» не пишем: видно в строке «Сеть», а пропала — шапка красная.
    int rw = right.length() ? textW(FONT_S, right.c_str()) : 0;
    int iw = red ? 0 : icons(d, 0, 0, false);
    if (!title && !red && d.timeOk) {                     // дата — если влезает рядом со значками
        struct tm t;
        gmtime_r(&d.loc, &t);
        char b[16];
        snprintf(b, sizeof(b), "%d %s", t.tm_mday, MON[t.tm_mon]);
        if (x + textW(FONT_S, b) + 8 + iw + 6 + rw <= W - 10) text(FONT_S, x, 26, b, dc);
    }
    if (title) text(FONT_S, x, 26, title, C_TEXT, W - 10 - rw - iw - 12 - x);
    if (iw) icons(d, W - 10 - rw - iw - (rw ? 6 : 0), 27, true);
    if (rw) textR(FONT_S, W - 10, 26, right.c_str(), dc);
    fill(0, 37, W, 1, C_LINE);
}

// 1. Главный: батарея крупно, солнце, нагрузка, сеть, режим, что дальше.
static void pageMain(const DData& d) {
    header(d, nullptr);
    float bw = d.battV * d.battI;
    text(FONT_S, 10, 58, "Батарея", C_DIM);
    if (d.battV > 1) {
        char b[16];
        if (d.socKnown) snprintf(b, sizeof(b), "%d%%", (int)lroundf(d.soc));
        else { snprintf(b, sizeof(b), "%.1f", d.battV); for (char* p = b; *p; p++) if (*p == '.') *p = ','; }
        uint16_t sc = !d.socKnown ? C_TEXT : d.soc < 20 ? C_RED : d.soc < 50 ? C_AMBER : C_GREEN;
        text(FONT_L, 8, 116, b, sc);
        textR(FONT_M, W - 10, 80, (dec(d.battV, 1) + " В").c_str(), C_TEXT);
        bool chg = bw > 20, dis = bw < -20;
        String p = String(chg ? "+" : dis ? "−" : "") + watts(bw);
        textR(FONT_M, W - 10, 106, p.c_str(), chg ? C_GREEN : dis ? C_AMBER : C_DIM);
        String st = chg ? "заряд" : dis ? "разряд" : "покой";
        if (d.etaS && dis) st = "хватит на " + span(d.etaS);
        if (d.etaS && chg) st = "до 100% — " + span(d.etaS);
        textR(FONT_S, W - 10, 128, st.c_str(), dis && d.etaS && d.etaS < 3600 ? C_RED : C_DIM);
        if (d.socKnown) {
            fill(10, 136, W - 20, 8, C_LINE);
            fill(10, 136, (int)((W - 20) * constrain(d.soc, 0.0f, 100.0f) / 100), 8, sc);
        }
    } else text(FONT_M, 10, 100, "нет данных", C_DIM);

    const String dash = "—";
    int y = 150;
    row(y, "Солнце", d.invOk ? watts(d.pvW) : dash, d.pvW > 20 ? C_SUN : C_TEXT, "сегодня " + kwh(d.pvWh));
    y += 44;
    row(y, "Нагрузка", d.invOk ? watts(d.outW) : dash, C_TEXT, "сегодня " + kwh(d.loadWh));
    y += 44;
    String gv = String((int)lroundf(d.gridV)) + " В · ";
    row(y, "Сеть", !d.invOk ? dash : d.gridOn ? watts(d.gridW) : String("нет"), d.gridOn ? C_BLUE : C_RED,
        (d.gridOn ? gv : String("")) + "сегодня " + kwh(d.gridWh));

    // Низ: режим (или связь, авария) и что сделает автоматика.
    String f;
    uint16_t fc = C_DIM;
    if (!d.invOk) { f = "нет связи с инвертором"; fc = C_RED; }
    else if (d.fault) { f = String("Авария ") + d.fault + ": " + invCodeText(d.fault); fc = C_RED; }
    else {
        static const char* const OPP[] = { "GRD", "PU", "PBG" };
        static const char* const CHP[] = { "PV", "PNG", "OPV" };
        f = invModeName(d.mode);
        if (f.startsWith("работа ")) f = f.substring(13);   // «работа от сети» → «от сети»: место внизу дорого
        if (d.prioOut < 3) f += String(" · ") + OPP[d.prioOut];
        if (d.prioChg < 3) f += String("/") + CHP[d.prioChg];
        if (d.tempC) f += " · " + String(d.tempC) + " °C";
    }
    if (d.next[0]) {
        text(FONT_S, 10, H - 25, f.c_str(), fc, W - 20);
        text(FONT_S, 10, H - 5, d.next, C_TEXT, W - 20);
    } else text(FONT_S, 10, H - 6, f.c_str(), fc, W - 20);
}

// 2. График за 24 часа: солнце заливкой, нагрузка и заряд линиями.
static void pageGraph(const DData& d) {
    header(d, "24 часа");
    if (!d.gOk) { text(FONT_M, 10, 100, "нет времени —", C_DIM); text(FONT_M, 10, 128, "графика нет", C_DIM); return; }
    const int X0 = 10, PW = W - 20, Y0 = 62, PH = 170, YB = Y0 + PH;
    static int16_t pv[W], ld[W], sc[W];
    int mx = 500;
    for (int c = 0; c < PW; c++) {
        int a = c * NPT / PW, b = (c + 1) * NPT / PW;
        long sp = 0, sl = 0, ss = 0; int np = 0, ns = 0;
        for (int i = a; i < b; i++) {
            if (d.gPv[i] >= 0) { sp += d.gPv[i]; sl += d.gLoad[i]; np++; }
            if (d.gSoc[i] <= 100) { ss += d.gSoc[i]; ns++; }
        }
        pv[c] = np ? sp / np : -1;
        ld[c] = np ? sl / np : -1;
        sc[c] = ns ? ss / ns : -1;
        if (np) mx = max(mx, (int)max(pv[c], ld[c]));
    }
    int top = (mx + 499) / 500 * 500;                    // шкала — кратно 0,5 кВт
    for (int k = 1; k <= 2; k++) fill(X0, YB - PH * k / 2, PW, 1, C_LINE);
    fill(X0, YB, PW, 1, C_DIM);
    text(FONT_S, X0, Y0 - 6, ((top % 1000 ? dec(top / 1000.0f, 1) : String(top / 1000)) + " кВт").c_str(), C_DIM);
    // Часы внизу: метки на 0, 6, 12, 18.
    uint32_t endSlot = d.loc / 300;
    int lastH = -1;
    for (int c = 0; c < PW; c++) {
        uint32_t slot = endSlot - (NPT - 1) + (uint32_t)c * NPT / PW;
        int h = (slot * 300 % 86400) / 3600;
        if (lastH >= 0 && h != lastH && h % 6 == 0) {
            fill(X0 + c, YB + 1, 1, 4, C_DIM);
            char b[4];
            snprintf(b, sizeof(b), "%02d", h);
            text(FONT_S, X0 + c - textW(FONT_S, b) / 2, YB + 20, b, C_DIM);
        }
        lastH = h;
    }
    for (int c = 0; c < PW; c++) {
        if (pv[c] <= 0) continue;
        int y = YB - (int)((long)pv[c] * PH / top);
        fill(X0 + c, y, 1, YB - y, C_SUNFILL);
        fill(X0 + c, y, 1, 2, C_SUN);
    }
    for (int c = 1; c < PW; c++) {
        if (sc[c] >= 0 && sc[c - 1] >= 0)
            line(X0 + c - 1, YB - sc[c - 1] * PH / 100, X0 + c, YB - sc[c] * PH / 100, C_GREEN);
        if (ld[c] >= 0 && ld[c - 1] >= 0)
            line(X0 + c - 1, YB - (int)((long)ld[c - 1] * PH / top), X0 + c, YB - (int)((long)ld[c] * PH / top), C_BLUE);
    }
    int y = YB + 44, x = X0;
    fill(x, y - 9, 10, 8, C_SUN);   x = text(FONT_S, x + 14, y, "солнце", C_DIM) + 12;
    fill(x, y - 6, 10, 2, C_BLUE);  x = text(FONT_S, x + 14, y, "дом", C_DIM) + 12;
    fill(x, y - 6, 10, 2, C_GREEN); text(FONT_S, x + 14, y, "заряд", C_DIM);
    text(FONT_S, X0, H - 8, ("сегодня " + kwhv(d.pvWh) + " / " + kwh(d.loadWh)).c_str(), C_TEXT, W - 20);
}

// 3. Неделя: столбики по дням — солнце и дом, в доме — сколько из сети.
static void pageWeek(const DData& d) {
    header(d, "Неделя");
    const int X0 = 10, PW = W - 20, Y0 = 66, PH = 170, YB = Y0 + PH, GW = PW / 7;
    float mx = 1000, sp = 0, sl = 0;
    int n = 0;
    for (int k = 0; k < 7; k++) if (d.wOk[k]) { mx = max(mx, max(d.wPv[k], d.wLoad[k])); sp += d.wPv[k]; sl += d.wLoad[k]; n++; }
    float top = ceilf(mx / 5000) * 5000;                  // шкала — кратно 5 кВт·ч
    for (int k = 1; k <= 2; k++) fill(X0, YB - PH * k / 2, PW, 1, C_LINE);
    fill(X0, YB, PW, 1, C_DIM);
    text(FONT_S, X0, Y0 - 8, (String((int)(top / 1000)) + " кВт·ч").c_str(), C_DIM);
    auto h = [&](float wh) { return (int)(wh * PH / top); };
    for (int k = 0; k < 7; k++) {
        int x = X0 + k * GW;
        if (d.wOk[k]) {
            fill(x + 4, YB - h(d.wPv[k]), 11, h(d.wPv[k]), C_SUN);
            fill(x + 16, YB - h(d.wLoad[k]), 11, h(d.wLoad[k]), C_BLUE);
            fill(x + 16, YB - h(d.wGrid[k]), 11, h(d.wGrid[k]), C_AMBER);
        }
        const char* dn = DOW[d.wDow[k] % 7];
        text(FONT_S, x + GW / 2 - textW(FONT_S, dn) / 2, YB + 20, dn, k == 6 ? C_TEXT : C_DIM);
    }
    int y = YB + 44, x = X0;
    fill(x, y - 9, 10, 8, C_SUN);   x = text(FONT_S, x + 14, y, "солнце", C_DIM) + 10;
    fill(x, y - 9, 10, 8, C_BLUE);  x = text(FONT_S, x + 14, y, "дом", C_DIM) + 10;
    fill(x, y - 9, 10, 8, C_AMBER); text(FONT_S, x + 14, y, "из сети", C_DIM);
    if (n) text(FONT_S, X0, H - 8, ("в день: солнце " + kwhv(sp / n) + ", дом " + kwhv(sl / n)).c_str(), C_TEXT, W - 20);
}

// 4. Итоги: вчера, неделя, месяц; отключения; прогноз.
static void pageTotals(const DData& d) {
    header(d, "Итоги");
    static const char* const T[3] = { "Вчера", "7 дней", "Месяц" };
    const int CX[3] = { 10, 88, 166 };
    text(FONT_S, CX[0], 58, "солнце", C_SUN);
    text(FONT_S, CX[1], 58, "дом", C_TEXT);
    text(FONT_S, CX[2], 58, "из сети", C_BLUE);
    for (int k = 0; k < 3; k++) {
        int y = 64 + k * 74;
        int x = text(FONT_M, 10, y + 21, T[k], C_TEXT) + 8;
        if (!d.tOk[k]) { text(FONT_S, x, y + 21, "нет данных", C_DIM); fill(10, y + 70, W - 20, 1, C_LINE); continue; }
        if (d.price > 0) textR(FONT_M, W - 10, y + 21, (String((long)lroundf(d.tPv[k] / 1000 * d.price)) + " ₽").c_str(), C_GREEN);
        const float v[3] = { d.tPv[k], d.tLoad[k], d.tGrid[k] };
        const uint16_t c[3] = { C_SUN, C_TEXT, C_BLUE };
        for (int i = 0; i < 3; i++) text(FONT_M, CX[i], y + 45, kwhv(v[i]).c_str(), c[i]);
        String o = d.tOut[k] ? "отключений " + String(d.tOut[k]) + " · " + span(d.tOutS[k]) : String("отключений не было");
        text(FONT_S, 10, y + 64, o.c_str(), d.tOut[k] ? C_AMBER : C_DIM, W - 20);
        fill(10, y + 70, W - 20, 1, C_LINE);
    }
    if (d.fc0 >= 0) text(FONT_S, 10, H - 24, ("сегодня солнце " + kwhv(d.pvWh) + " из " + dec(d.fc0, 1)).c_str(), C_TEXT, W - 20);
    if (d.fc1 >= 0) text(FONT_S, 10, H - 5, ("прогноз на завтра " + dec(d.fc1, 1) + " кВт·ч").c_str(), C_TEXT, W - 20);
}

// 5. Батарея подробно: то, что BMS сообщает инвертору, износ, ячейки.
static void pageBatt(const DData& d) {
    header(d, "Батарея");
    if (!d.bOk) { text(FONT_M, 10, 100, "нет данных от BMS", C_DIM); return; }
    int y = 42, lim = d.cells ? 170 : 280;                // с ячейками — им нижняя часть
    String t = d.tMin != d.tMax ? String(d.tMin) + "…" + String(d.tMax) + " °C" : String(d.tMax) + " °C";
    row2(y, "Температура", t, d.tMax >= 45 || d.tMin <= 0 ? C_AMBER : C_TEXT); y += 34;
    row2(y, "Ток", String(d.battI > 0.05f ? "+" : d.battI < -0.05f ? "−" : "") + dec(fabsf(d.battI), 1) + " А",
         d.battI > 0.05f ? C_GREEN : d.battI < -0.05f ? C_AMBER : C_TEXT); y += 34;
    if (d.socBms >= 0 && y < lim) { row2(y, "Заряд по BMS", String((int)lroundf(d.socBms)) + " %", C_TEXT); y += 34; }
    if (d.chgLimA >= 0 && y < lim) { row2(y, "Лимит тока заряда", dec(d.chgLimA, 0) + " А", C_TEXT); y += 34; }
    if (d.cvV > 0 && y < lim) { row2(y, "Напряжение заряда", dec(d.cvV, 1) + " В", C_TEXT); y += 34; }
    if (d.capAh > 0 && y < lim) {
        String c = dec(d.capAh, 0) + " А·ч";
        if (d.capEst > 0) c = dec(d.capEst, 0) + " из " + c;
        row2(y, "Ёмкость", c, C_TEXT); y += 34;
    }
    if (d.capAh > 0 && y < lim) { row2(y, "Циклов (полных)", dec(d.ahOut / d.capAh, 1), C_TEXT); y += 34; }
    if (d.cells) {
        // Столбики ячеек: шкала от самой низкой до самой высокой с запасом,
        // слабейшая — красным, сильнейшая — синим.
        int n = min((int)d.cells, (int)MAX_CELLS), Y0 = 220, PH = 66, YB = Y0 + PH, bw = (W - 20) / n;
        int lo = d.cMin - 10, hi = d.cMax + 10;
        for (int i = 0; i < n; i++) {
            uint16_t mv = d.cellMv[i];
            if (!mv) continue;
            int hh = 6 + (mv - lo) * (PH - 6) / max(1, hi - lo);
            uint16_t c = mv == d.cMin ? C_RED : mv == d.cMax ? C_BLUE : C_GREEN;
            fill(10 + i * bw, YB - hh, max(1, bw - 1), hh, c);
        }
        text(FONT_S, 10, Y0 - 6, ("ячейки, " + String(n) + " шт.").c_str(), C_DIM);
        String s = dec(d.cMin / 1000.0f, 3) + "–" + dec(d.cMax / 1000.0f, 3) + ", разброс " + String(d.cMax - d.cMin) + " мВ";
        text(FONT_S, 10, YB + 18, s.c_str(), d.cMax - d.cMin > 50 ? C_AMBER : C_TEXT, W - 20);
    }
    if (d.bFault || d.bWarn) {
        String s = d.bFault ? "BMS: авария " + String(d.bFault) : "BMS: предупреждение " + String(d.bWarn);
        fill(0, H - 24, W, 24, C_BG);
        text(FONT_S, 10, H - 6, s.c_str(), d.bFault ? C_RED : C_AMBER, W - 20);
    }
}

// 6. Модуль: сеть, работа, последние события.
static void pageModule(const DData& d) {
    header(d, "Модуль");
    int y = 42;
    row2(y, d.ap ? "Точка доступа" : "Адрес", d.ip, C_TEXT); y += 34;
    if (!d.ap) {
        uint16_t c = d.rssi > -67 ? C_GREEN : d.rssi > -80 ? C_AMBER : C_RED;
        String w = d.rssi ? String(d.rssi) + " дБм" : String("нет");
        if (d.rssi && d.inet == 0) w += ", без интернета";
        row2(y, "Wi-Fi", w, d.rssi && d.inet != 0 ? c : C_RED); y += 34;
    }
    uint32_t m = d.upS / 60;
    String up = m >= 1440 ? String(m / 1440) + " д " + String(m / 60 % 24) + " ч" : String(m / 60) + " ч " + String(m % 60) + " мин";
    row2(y, "Работает", up, C_TEXT); y += 34;
    row2(y, "Память, мин.", String(d.heapMin / 1024) + " КБ", d.heapMin < 40000 ? C_AMBER : C_TEXT); y += 34;
    row2(y, "Прошивка", d.fw, C_TEXT); y += 34;
    y += 16;
    text(FONT_S, 10, y, "События", C_DIM);
    for (uint8_t i = 0; i < d.nEv; i++) {
        y += 21;
        const DEv& e = d.ev[i];
        uint16_t c = e.lv == 'a' ? C_RED : e.lv == 'w' ? C_AMBER : e.lv == 'g' ? C_GREEN : C_TEXT;
        int x = text(FONT_S, 10, y, e.when, C_DIM) + 6;
        text(FONT_S, x, y, e.text, c, W - 10 - x);
    }
}

static void render(const DData& d) {
    fill(0, 0, W, H, C_BG);
    switch (d.page) {
    case 1:  pageGraph(d);  break;
    case 2:  pageWeek(d);   break;
    case 3:  pageTotals(d); break;
    case 4:  pageBatt(d);   break;
    case 5:  pageModule(d); break;
    default: pageMain(d);   break;
    }
}

// ------------------------------------------------------------ задача
static DData s_rd;                                        // копия для задачи: 4 КБ — не на стек
static void task(void*) {
    panelInit();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        if (s_reinit) { s_reinit = false; panelInit(); }
        if (!s_have) continue;
        portENTER_CRITICAL(&s_mux);
        memcpy(&s_rd, &s_d, sizeof(DData));
        portEXIT_CRITICAL(&s_mux);
        // Погашенный экран не рисуем — только держим подсветку выключенной.
        if (s_rd.bl) { render(s_rd); push(); }
        if (s_rd.bl != s_blNow) { s_blNow = s_rd.bl; ledcWrite(PIN_BL, (uint32_t)s_rd.bl * 255 / 100); }
    }
}

static void load() {
    Preferences p;
    if (!p.begin("disp", true)) return;
    s_day = p.getUChar("day", 80);
    s_night = p.getUChar("night", 15);
    s_flip = p.getUChar("flip", 0);
    s_inv = p.getUChar("inv", 1);
    s_bgr = p.getUChar("bgr", 0);
    s_sleep = p.getUChar("sleep", 0);
    p.end();
}

void dispStart() {
    if (!psramFound()) { Serial.println("[дисплей] нет PSRAM — кадру негде жить, выключен"); return; }
    s_fb = (uint16_t*)ps_malloc(W * H * 2);
    if (!s_fb) return;
    load();
    pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_DC, OUTPUT);
    pinMode(PIN_RST, OUTPUT);
    pinMode(PIN_BTN, INPUT_PULLUP);
    ledcAttach(PIN_BL, 5000, 8);
    ledcWrite(PIN_BL, 0);                                 // без мусора при старте
    s_spi.begin(PIN_SCK, -1, PIN_MOSI, -1);
    xTaskCreatePinnedToCore(task, "disp", 4096, nullptr, 1, &s_task, 0);
    Serial.println("[дисплей] ST7789 240×320, SPI 40 МГц, кнопка экранов — GPIO 4");
}

// Кнопка: короткое нажатие — true.
static bool button() {
    static bool raw = true, st = true;
    static uint32_t at = 0;
    bool v = digitalRead(PIN_BTN);
    if (v != raw) { raw = v; at = millis(); return false; }
    if (v == st || millis() - at < 30) return false;
    st = v;
    return !v;
}

// Последние события из журнала — разбором его же JSON, чтобы не держать
// вторую выдачу.
static uint8_t events(DEv* out, uint8_t n, bool timeOk, time_t loc) {
    String j = evJson(n);
    uint8_t k = 0;
    int p = 0;
    auto field = [&](const char* key, int from, int to) -> String {
        int a = j.indexOf(key, from);
        if (a < 0 || a > to) return "";
        a += strlen(key);
        String r;
        for (int i = a; i < (int)j.length() && j[i] != '"'; i++) {
            if (j[i] == '\\' && i + 1 < (int)j.length()) i++;
            r += j[i];
        }
        return r;
    };
    while (k < n && (p = j.indexOf('{', p)) >= 0) {
        int e = j.indexOf("\"}", p);
        if (e < 0) break;
        String when = field("\"when\":\"", p, e), lv = field("\"level\":\"", p, e);
        String t = field("\"text\":\"", p, e), det = field("\"detail\":\"", p, e);
        if (det.length()) t += ": " + det;
        DEv& o = out[k++];
        // «02.11 18:58:12» → «18:58», если сегодня; иначе «02.11».
        String hm = when.length() >= 11 ? when.substring(6, 11) : String("—");
        if (when.length() >= 5 && timeOk) {
            struct tm lt;
            gmtime_r(&loc, &lt);
            char today[6];
            snprintf(today, sizeof(today), "%02d.%02d", lt.tm_mday, lt.tm_mon + 1);
            if (when.substring(0, 5) != today) hm = when.substring(0, 5);
        }
        strlcpy(o.when, hm.c_str(), sizeof(o.when));
        strlcpy(o.text, t.c_str(), sizeof(o.text));
        o.lv = lv == "alarm" ? 'a' : lv == "warn" ? 'w' : lv == "good" ? 'g' : 'i';
        p = e + 2;
    }
    return k;
}

// Что сделает автоматика: ближайшее из расписания и ночной зарядки —
// «в 16:41 включу GRD», «завтра в 07:00 включу PBG», «в 23:00 — ночной заряд от сети».
static void nextAction(DData& d) {
    d.next[0] = 0;
    if (!d.timeOk) return;
    struct tm lt;
    gmtime_r(&d.loc, &lt);
    int nowMin = lt.tm_hour * 60 + lt.tm_min, best = -1, in;
    String what, sw;
    if (schedNext(in, sw)) {
        best = in;
        sw.replace("OPP → ", "");
        sw.replace("CHP → ", "CHP ");
        what = sw;
    }
    const Config& c = cfg();
    if (c.autoOn && c.autoFrom != c.autoTo) {
        if (d.night) {
            int m = (c.autoTo * 60 - nowMin + 1440) % 1440;
            if (best < 0 || m < best) { best = m; what = "конец ночного заряда"; }
        } else if (d.fc1 >= 0 && d.fc1 < c.autoBelow) {
            int m = (c.autoFrom * 60 - nowMin + 1440) % 1440;
            if (best < 0 || m < best) { best = m; what = "ночной заряд от сети"; }
        }
    }
    if (best < 0) return;
    int ahead = (nowMin + best) / 1440, at = (nowMin + best) % 1440;
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", at / 60, at % 60);
    String when = ahead == 0 ? "" : ahead == 1 ? "завтра " : String(DOW[((lt.tm_wday + 6) % 7 + ahead) % 7]) + " ";
    bool sched = what.length() <= 8;                      // «GRD», «CHP PNG» — из расписания
    strlcpy(d.next, (when + "в " + b + (sched ? " включу " : " — ") + what).c_str(), sizeof(d.next));
}

void dispLoop() {
    if (!s_task) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (button()) {
        bool dark = s_blNow == 0;
        s_lastPress = now;
        if (dark) s_wakeUntil = now + WAKE_MS;            // погашен — только разбудить
        else s_page = (s_page + 1) % NPAGE;
        last = 0;
        s_heavyAt = 0;
    }
    if (s_page && now - s_lastPress > HOME_MS) { s_page = 0; last = 0; }
    if (last && now - last < 1000) return;
    last = now | 1;

    static DData d;                                       // тяжёлое держится между кадрами
    d.timeOk = statsTimeValid();
    d.loc = time(nullptr) + TZ_SEC;
    InvState v = invSnapshot();
    BmsState b = stateSnapshot();
    d.invOk = v.linked && v.fresh;
    d.gridOn = v.gridOn; d.mode = v.mode; d.prioOut = v.prioOut; d.prioChg = v.prioChg;
    d.fault = v.faultCode; d.tempC = v.tempC;
    d.gridV = v.gridV; d.gridW = v.gridW; d.outW = v.outW; d.pvW = v.pvW;
    d.battV = b.voltage; d.battI = b.current; d.soc = b.soc; d.socKnown = b.socKnown;
    statsToday(d.pvWh, d.gridWh, d.loadWh);
    if (v.pvKwhToday >= 0) d.pvWh = v.pvKwhToday * 1000;          // как на странице: счётчик инвертора, если есть
    if (v.gridKwhToday >= 0) d.gridWh = v.gridKwhToday * 1000;
    if (v.loadKwhToday >= 0) d.loadWh = v.loadKwhToday * 1000;
    // Хватит / до полного — по среднему току за 3 мин, как в боте.
    d.etaS = d.socKnown ? battEtaS(d.soc, b.capAh, statsBattAvgA(), cfg().tgSocLow) : 0;
    d.night = weatherChargeActive();
    d.ap = netIsAp();
    d.rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    d.wifi = d.ap ? 3 : !d.rssi ? 0 : d.rssi > -80 ? 2 : 1;
    d.inet = tgOnline();

    // Тревога: нет сети, авария инвертора или заряд ниже порога бота —
    // главный экран, полная яркость первые 10 мин. Кончилась — через минуту
    // всё как обычно.
    bool gridLost = d.invOk && !d.gridOn;
    if (gridLost && !s_gridLostAt) s_gridLostAt = now | 1;
    if (!gridLost) s_gridLostAt = 0;
    d.gridLostS = s_gridLostAt ? (now - s_gridLostAt) / 1000 : 0;
    bool alert = gridLost || (d.invOk && d.fault) || (d.socKnown && d.battV > 1 && d.soc < cfg().tgSocLow);
    if (alert) {
        s_alertOffAt = 0;
        if (!s_alertAt) { s_alertAt = now | 1; s_page = 0; s_lastPress = now; s_heavyAt = 0; }
    } else if (s_alertAt) {
        if (!s_alertOffAt) s_alertOffAt = now | 1;
        if (now - s_alertOffAt > 60000) s_alertAt = s_alertOffAt = 0;
    }
    d.page = s_page;

    // Яркость: день/ночь; хранитель экрана гасит через s_sleep минут без
    // нажатий (не при тревоге); кнопка будит на минуту.
    uint8_t pct = s_day;
    if (d.timeOk) { int h = (d.loc / 3600) % 24; if (h >= 22 || h < 7) pct = s_night; }
    bool woke = (int32_t)(s_wakeUntil - now) > 0;
    if (s_sleep && !s_alertAt && !woke && now - s_lastPress > (uint32_t)s_sleep * 60000) pct = 0;
    if (!pct && (woke || s_alertAt)) pct = s_day ? s_day : 50;
    if (s_alertAt && now - s_alertAt < ALERT_BRIGHT_MS) pct = 100;
    d.bl = pct;

    // Тяжёлое (файлы статистики, журнал) — для открытого экрана, не чаще раза в 30 с.
    bool heavy = !s_heavyAt || now - s_heavyAt > 30000;
    if (heavy) { s_heavyAt = now | 1; d.fc0 = weatherPvKwh(0); d.fc1 = weatherPvKwh(1); nextAction(d); }
    uint32_t day = statsDay();
    if (heavy && s_page == 1) d.gOk = statsPower24(d.gPv, d.gLoad, d.gSoc);
    if (heavy && s_page == 2) {
        for (int k = 0; k < 7; k++) {
            StatSum s;
            uint32_t dd = day - 6 + k;
            d.wOk[k] = day && statsSum(dd, dd, s) && s.dataS > 0;
            d.wPv[k] = d.wOk[k] ? s.pvWh : 0; d.wLoad[k] = d.wOk[k] ? s.loadWh : 0; d.wGrid[k] = d.wOk[k] ? s.gridWh : 0;
            d.wDow[k] = (dd + 3) % 7;                     // 1.1.1970 — четверг; пн = 0
        }
    }
    if (heavy && s_page == 3) {
        struct tm lt;
        gmtime_r(&d.loc, &lt);
        const uint32_t from[3] = { day - 1, day - 6, day - (lt.tm_mday - 1) }, to[3] = { day - 1, day, day };
        for (int k = 0; k < 3; k++) {
            StatSum s;
            d.tOk[k] = day && statsSum(from[k], to[k], s) && s.dataS > 0;
            d.tPv[k] = d.tOk[k] ? s.pvWh : 0; d.tLoad[k] = d.tOk[k] ? s.loadWh : 0; d.tGrid[k] = d.tOk[k] ? s.gridWh : 0;
            d.tOut[k] = d.tOk[k] ? s.outages : 0; d.tOutS[k] = d.tOk[k] ? s.outageS : 0;
        }
        d.price = cfg().price;
    }
    if (s_page == 4) {
        d.bOk = b.voltage > 1;
        d.tMin = b.tempMin; d.tMax = b.tempMax;
        d.chgLimA = b.chgLimitA; d.cvV = b.cvV; d.socBms = b.socBms; d.capAh = b.capAh;
        d.cells = b.cellCount; d.cMin = b.cellMinMv; d.cMax = b.cellMaxMv;
        memcpy(d.cellMv, b.cellMv, sizeof(d.cellMv));
        d.bFault = b.bmsFault; d.bWarn = b.bmsWarn;
        battLifeInfo(d.ahOut, d.capEst);
    }
    if (s_page == 5) {
        strlcpy(d.ip, netIp().c_str(), sizeof(d.ip));
        d.upS = now / 1000;
        d.heapMin = ESP.getMinFreeHeap();
        String fw = fwBuild();                            // «02.11.2026 18:57, ab52422» → «ab52422 · 02.11»
        int c = fw.indexOf(", ");
        if (c >= 16) fw = fw.substring(c + 2) + " · " + fw.substring(0, 5);
        strlcpy(d.fw, fw.c_str(), sizeof(d.fw));
        if (heavy) d.nEv = events(d.ev, 4, d.timeOk, d.loc);
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(&s_d, &d, sizeof(DData));
    s_have = true;
    portEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_task);
}

String dispJson() {
    return "{\"on\":" + String(s_task ? "true" : "false") + ",\"day\":" + String(s_day) + ",\"night\":" + String(s_night) +
           ",\"flip\":" + String(s_flip) + ",\"inv\":" + String(s_inv) + ",\"bgr\":" + String(s_bgr) +
           ",\"sleep\":" + String(s_sleep) + "}";
}

String dispSet(int day, int night, int flip, int inv, int bgr, int sleep) {
    if (day > 100 || night > 100) return "яркость — от 0 до 100 %";
    if (sleep > 120) return "гасить — не позже чем через 120 мин";
    if (day >= 0) s_day = day;
    if (night >= 0) s_night = night;
    if (sleep >= 0) s_sleep = sleep;
    bool re = false;
    if (flip >= 0 && flip != s_flip) { s_flip = flip ? 1 : 0; re = true; }
    if (inv >= 0 && inv != s_inv) { s_inv = inv ? 1 : 0; re = true; }
    if (bgr >= 0 && bgr != s_bgr) { s_bgr = bgr ? 1 : 0; re = true; }
    s_lastPress = millis();                               // после настройки экран не гаснет сразу
    if (re) s_reinit = true;
    Preferences p;
    if (p.begin("disp", false)) {
        p.putUChar("day", s_day); p.putUChar("night", s_night); p.putUChar("sleep", s_sleep);
        p.putUChar("flip", s_flip); p.putUChar("inv", s_inv); p.putUChar("bgr", s_bgr);
        p.end();
    }
    return "";
}
#endif
