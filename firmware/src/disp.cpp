// Дисплей ST7789. Описание и подключение — в disp.h.
#include "disp.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
void   dispStart() {}
void   dispLoop() {}
String dispJson() { return "{\"on\":false}"; }
String dispSet(int, int, int, int, int) { return "дисплея на этой плате нет"; }
#else

#include <SPI.h>
#include <Preferences.h>
#include <time.h>
#include "state.h"
#include "stats.h"
#include "inv_codes.h"
#include "disp_font.h"

static const int PIN_SCK = 12, PIN_MOSI = 11, PIN_CS = 10, PIN_DC = 13, PIN_RST = 16, PIN_BL = 14;
static const int W = 240, H = 320;
static const uint32_t SPI_HZ = 40000000;
static const int32_t TZ_SEC = 3 * 3600;                  // как везде в прошивке

static SPIClass  s_spi(FSPI);
static uint16_t* s_fb = nullptr;                         // кадр, байты уже в порядке для SPI
static TaskHandle_t s_task = nullptr;

// Настройки: яркость в %, остальное 0/1. Меняются со страницы — задача
// перечитывает их перед кадром.
static volatile uint8_t s_day = 80, s_night = 15, s_flip = 0, s_inv = 1, s_bgr = 0;
static volatile bool s_reinit = false;

// Что показывать: собирает loop(), рисует задача.
struct DData {
    bool     timeOk;
    time_t   loc;
    bool     invOk, gridOn, socKnown;
    uint8_t  mode, prioOut, prioChg, fault;
    int16_t  tempC;
    float    gridV, gridW, outW, pvW, battV, battI, soc;
    float    pvWh, gridWh, loadWh;
};
static DData s_d;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_have = false;

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
                      C_RED = rgb(255, 77, 79), C_SUN = rgb(255, 204, 51), C_BLUE = rgb(77, 163, 255);

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

// y — базовая линия. Возвращает x после текста.
static int text(const DFont& f, int x, int y, const char* s, uint16_t c) {
    while (*s) {
        const DGlyph* g = glyph(f, utf8(s));
        if (!g) continue;
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

// 1234 → «1,23 кВт», 850 → «850 Вт»
static String watts(float w) {
    float a = fabsf(w);
    String r = a >= 1000 ? String(a / 1000, a >= 10000 ? 1 : 2) + " кВт" : String((int)lroundf(a)) + " Вт";
    r.replace('.', ',');
    return r;
}
static String kwh(float wh) { String r = String(wh / 1000, 1); r.replace('.', ','); return r + " кВт·ч"; }

static void row(int y, const char* label, const String& value, uint16_t vc, const String& sub) {
    text(FONT_S, 10, y + 18, label, C_DIM);
    if (sub.length()) text(FONT_S, 10, y + 38, sub.c_str(), C_DIM);
    textR(FONT_M, W - 10, y + 20, value.c_str(), vc);
    fill(10, y + 45, W - 20, 1, C_LINE);
}

static void render(const DData& d) {
    fill(0, 0, W, H, C_BG);
    static const char* const MON[] = { "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек" };

    // Шапка: время, дата, сеть.
    if (d.timeOk) {
        struct tm t;
        gmtime_r(&d.loc, &t);
        char b[24];
        snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
        int x = text(FONT_M, 10, 26, b, C_TEXT);
        snprintf(b, sizeof(b), "%d %s", t.tm_mday, MON[t.tm_mon]);
        text(FONT_S, x + 8, 26, b, C_DIM);
    }
    if (d.invOk) textR(FONT_S, W - 10, 26, d.gridOn ? "сеть есть" : "сети нет", d.gridOn ? C_GREEN : C_RED);
    fill(0, 36, W, 1, C_LINE);

    // Батарея: заряд крупно, справа напряжение и мощность.
    float bw = d.battV * d.battI;
    text(FONT_S, 10, 58, "Батарея", C_DIM);
    if (d.battV > 1) {
        char b[16];
        if (d.socKnown) snprintf(b, sizeof(b), "%d%%", (int)lroundf(d.soc));
        else { snprintf(b, sizeof(b), "%.1f", d.battV); for (char* p = b; *p; p++) if (*p == '.') *p = ','; }
        uint16_t sc = !d.socKnown ? C_TEXT : d.soc < 20 ? C_RED : d.soc < 50 ? C_AMBER : C_GREEN;
        text(FONT_L, 8, 116, b, sc);
        String v = String(d.battV, 1) + " В";
        v.replace('.', ',');
        textR(FONT_M, W - 10, 82, v.c_str(), C_TEXT);
        bool chg = bw > 20, dis = bw < -20;
        String p = String(chg ? "+" : dis ? "−" : "") + watts(bw);
        textR(FONT_M, W - 10, 110, p.c_str(), chg ? C_GREEN : dis ? C_AMBER : C_DIM);
        textR(FONT_S, W - 10, 130, chg ? "заряд" : dis ? "разряд" : "покой", C_DIM);
        if (d.socKnown) {
            fill(10, 140, W - 20, 8, C_LINE);
            fill(10, 140, (int)((W - 20) * constrain(d.soc, 0.0f, 100.0f) / 100), 8, sc);
        }
    } else text(FONT_M, 10, 100, "нет данных", C_DIM);

    // Строки: солнце, нагрузка, сеть.
    const String dash = "—";
    int y = 156;
    row(y, "Солнце", d.invOk ? watts(d.pvW) : dash, d.pvW > 20 ? C_SUN : C_TEXT, "сегодня " + kwh(d.pvWh));
    y += 48;
    row(y, "Нагрузка", d.invOk ? watts(d.outW) : dash, C_TEXT, "сегодня " + kwh(d.loadWh));
    y += 48;
    String gv = String((int)lroundf(d.gridV)) + " В · ";
    row(y, "Сеть", !d.invOk ? dash : d.gridOn ? watts(d.gridW) : String("нет"), d.gridOn ? C_BLUE : C_RED,
        (d.gridOn ? gv : String("")) + "сегодня " + kwh(d.gridWh));

    // Низ: связь, авария или режим.
    String f;
    uint16_t fc = C_DIM;
    if (!d.invOk) { f = "нет связи с инвертором"; fc = C_RED; }
    else if (d.fault) { f = String("Авария ") + d.fault + ": " + invCodeText(d.fault); fc = C_RED; }
    else {
        static const char* const MODE[] = { "—", "ожидание", "от сети", "от батареи", "обход", "авария" };
        static const char* const OPP[] = { "GRD", "PU", "PBG" };
        static const char* const CHP[] = { "PV", "PNG", "OPV" };
        f = MODE[d.mode < 6 ? d.mode : 0];
        if (d.prioOut < 3) f += String(" · ") + OPP[d.prioOut];
        if (d.prioChg < 3) f += String("/") + CHP[d.prioChg];
        if (d.tempC) f += " · " + String(d.tempC) + " °C";
    }
    text(FONT_S, 10, H - 10, f.c_str(), fc);
}

// ------------------------------------------------------------ задача
static uint8_t s_blNow = 255;
static void backlight(const DData& d) {
    uint8_t pct = s_day;
    if (d.timeOk) {
        int h = (d.loc / 3600) % 24;
        if (h >= 22 || h < 7) pct = s_night;
    }
    if (pct == s_blNow) return;
    s_blNow = pct;
    ledcWrite(PIN_BL, (uint32_t)pct * 255 / 100);
}

static void task(void*) {
    panelInit();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        if (s_reinit) { s_reinit = false; panelInit(); }
        if (!s_have) continue;
        DData d;
        portENTER_CRITICAL(&s_mux);
        d = s_d;
        portEXIT_CRITICAL(&s_mux);
        render(d);
        push();
        backlight(d);
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
    ledcAttach(PIN_BL, 5000, 8);
    ledcWrite(PIN_BL, 0);                                 // без мусора при старте
    s_spi.begin(PIN_SCK, -1, PIN_MOSI, -1);
    xTaskCreatePinnedToCore(task, "disp", 4096, nullptr, 1, &s_task, 0);
    Serial.println("[дисплей] ST7789 240×320, SPI 40 МГц");
}

void dispLoop() {
    static uint32_t last = 0;
    if (!s_task || millis() - last < 1000) return;
    last = millis();
    DData d = {};
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
    portENTER_CRITICAL(&s_mux);
    s_d = d;
    s_have = true;
    portEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_task);
}

String dispJson() {
    return "{\"on\":" + String(s_task ? "true" : "false") + ",\"day\":" + String(s_day) + ",\"night\":" + String(s_night) +
           ",\"flip\":" + String(s_flip) + ",\"inv\":" + String(s_inv) + ",\"bgr\":" + String(s_bgr) + "}";
}

String dispSet(int day, int night, int flip, int inv, int bgr) {
    if ((day >= 0 && day > 100) || (night >= 0 && night > 100)) return "яркость — от 0 до 100 %";
    if (day >= 0) s_day = day;
    if (night >= 0) s_night = night;
    bool re = false;
    if (flip >= 0 && flip != s_flip) { s_flip = flip ? 1 : 0; re = true; }
    if (inv >= 0 && inv != s_inv) { s_inv = inv ? 1 : 0; re = true; }
    if (bgr >= 0 && bgr != s_bgr) { s_bgr = bgr ? 1 : 0; re = true; }
    s_blNow = 255;                                        // яркость применится со следующим кадром
    if (re) s_reinit = true;
    Preferences p;
    if (p.begin("disp", false)) {
        p.putUChar("day", s_day); p.putUChar("night", s_night);
        p.putUChar("flip", s_flip); p.putUChar("inv", s_inv); p.putUChar("bgr", s_bgr);
        p.end();
    }
    if (s_task) xTaskNotifyGive(s_task);
    return "";
}
#endif
