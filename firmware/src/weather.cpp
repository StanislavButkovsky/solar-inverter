// Погода, прогноз выработки и ночная зарядка. Описание — в weather.h.
//
// Запрос к Open-Meteo — в своей задаче: TLS и ответ занимают секунды.
// Прогноз пишет задача, читает loop() и веб — под мьютексом s_mx.
// Обучение и автоматика — в loop(), рядом с переключением настроек.
#include "weather.h"
#include "cfg.h"
#include "state.h"
#include "stats.h"
#include "profile.h"
#include "tg.h"
#include "tg_roots.h"
#include "ota.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <algorithm>

static const int32_t  TZ_SEC   = 3 * 3600;                 // как везде в прошивке
static const uint32_t EVERY_MS = 3UL * 3600 * 1000;        // прогноз раз в 3 часа
static const uint32_t RETRY_MS = 10UL * 60 * 1000;
static const uint8_t  NK = 14;                             // дней для коэффициента панелей
static const float    PR = 0.8f;                           // кВт панелей → кВт·ч на кВт·ч/м²

static SemaphoreHandle_t s_tls = nullptr, s_mx = nullptr;
void tlsLock()   { if (s_tls) xSemaphoreTake(s_tls, portMAX_DELAY); }
void tlsUnlock() { if (s_tls) xSemaphoreGive(s_tls); }

static int16_t  s_rad[48];                // Вт/м² по часам: [0..23] — день s_fcDay, [24..47] — следующий
static float    s_sum[2] = { -1, -1 };    // кВт·ч/м² за сутки
static int32_t  s_fcDay = -1;             // местные сутки от 1970 для s_rad[0]
static uint32_t s_fetchedMs = 0;
static bool     s_have = false;
static String   s_err;
static volatile bool s_refresh = true;

static float    s_k[NK];                  // факт / радиация, по дням, новый — [0]
static uint8_t  s_kn = 0;
static int32_t  s_kDay = -1;              // за какой день коэффициент уже посчитан

static bool     s_active = false;         // CHP = PNG поставили мы
static int32_t  s_decided = -1;           // ночь, за которую решение принято
static int8_t   s_want = -1;              // какой CHP добиваемся; −1 — ничего
static uint32_t s_wantMs = 0;
static String   s_msg;                    // последнее решение автоматики — на страницу

// ------------------------------------------------------------------ время

static int32_t localDay() { return time(nullptr) > 1700000000 ? (int32_t)((time(nullptr) + TZ_SEC) / 86400) : -1; }

static int localHour() {
    time_t loc = time(nullptr) + TZ_SEC;
    struct tm lt;
    gmtime_r(&loc, &lt);
    return lt.tm_hour;
}

// Сутки от 1970 по календарной дате (алгоритм Хиннанта).
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

// ------------------------------------------------------------------ запрос

// Числа массива "key":[…] после позиции from; null — −1.
static int parseArr(const String& b, int from, const char* key, float* out, int max) {
    String k = String("\"") + key + "\":[";
    int p = b.indexOf(k, from);
    if (p < 0) return 0;
    p += k.length();
    int n = 0;
    while (n < max && p < (int)b.length() && b[p] != ']') {
        if (b.startsWith("null", p)) { out[n++] = -1; p += 4; }
        else { char* e; out[n++] = strtof(b.c_str() + p, &e); p = e - b.c_str(); }
        if (p < (int)b.length() && b[p] == ',') p++;
        else if (p < (int)b.length() && b[p] != ']') return n;   // мусор — что успели
    }
    return n;
}

static bool fetch() {
    char url[220];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&hourly=shortwave_radiation&daily=shortwave_radiation_sum&timezone=Etc%%2FGMT-3&forecast_days=2",
             cfg().lat, cfg().lon);
    WiFiClientSecure cli;
    cli.setCACert(TG_ROOT_CA);            // там же ISRG Root X1 — цепочка Open-Meteo, проверено 21.10.2026
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    String body, err;
    if (!http.begin(cli, url)) err = "не удалось начать запрос";
    else {
        int code = http.GET();
        if (code == 200) body = http.getString();
        else err = code > 0 ? "ответ " + String(code) : "нет связи с api.open-meteo.com (" + HTTPClient::errorToString(code) + ")";
        http.end();
    }
    cli.stop();

    int16_t rad[48];
    float sum[2] = { -1, -1 };
    int32_t day0 = -1;
    if (err.isEmpty()) {
        int h = body.indexOf("\"hourly\":{");
        int t = body.indexOf("\"time\":[\"", h);
        int y = 0, m = 0, d = 0;
        if (h >= 0 && t >= 0 && sscanf(body.c_str() + t + 9, "%4d-%2d-%2d", &y, &m, &d) == 3) day0 = daysFromCivil(y, m, d);
        float hv[48];
        int nh = h >= 0 ? parseArr(body, h, "shortwave_radiation", hv, 48) : 0;
        int dl = body.indexOf("\"daily\":{");
        int nd = dl >= 0 ? parseArr(body, dl, "shortwave_radiation_sum", sum, 2) : 0;
        if (day0 < 0 || nh < 48 || nd < 2) err = "ответ Open-Meteo не разобрал";
        for (int i = 0; i < 48; i++) rad[i] = i < nh && hv[i] >= 0 ? (int16_t)hv[i] : -1;
        for (int i = 0; i < 2; i++) if (sum[i] >= 0) sum[i] /= 3.6f;    // МДж/м² → кВт·ч/м²
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (err.isEmpty()) {
        memcpy(s_rad, rad, sizeof(rad));
        s_sum[0] = sum[0]; s_sum[1] = sum[1];
        s_fcDay = day0;
        s_fetchedMs = millis();
        s_have = true;
    }
    s_err = err;
    xSemaphoreGive(s_mx);
    Serial.printf("[wx] прогноз: %s\n", err.isEmpty() ? "получен" : err.c_str());
    return err.isEmpty();
}

static void task(void*) {
    uint32_t next = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (cfg().lat == 0 && cfg().lon == 0) continue;
        if (WiFi.status() != WL_CONNECTED || otaBusy()) continue;
        if (!s_refresh && (int32_t)(millis() - next) < 0) continue;
        s_refresh = false;
        tlsLock();
        bool ok = fetch();
        tlsUnlock();
        next = millis() + (ok ? EVERY_MS : RETRY_MS);
    }
}

void weatherRefresh() { s_refresh = true; }

// ------------------------------------------------------------------ прогноз

static float daySum(int32_t day) {
    if (!s_have || day < 0) return -1;
    if (day == s_fcDay) return s_sum[0];
    if (day == s_fcDay + 1) return s_sum[1];
    return -1;
}

static float kLearned() {
    if (s_kn < 3) return -1;
    float v[NK];
    memcpy(v, s_k, sizeof(float) * s_kn);
    std::sort(v, v + s_kn);
    uint8_t i = s_kn * 3 / 4;                                       // верхняя четверть
    return v[i < s_kn ? i : s_kn - 1];
}

static float kNow(String* src) {
    float k = kLearned();
    if (k > 0) { if (src) *src = "по факту, дней: " + String(s_kn); return k; }
    if (cfg().pvKwp > 0) { if (src) *src = "по мощности панелей"; return cfg().pvKwp * PR; }
    if (src) *src = "";
    return -1;
}

float weatherPvKwh(int8_t off) {
    int32_t d = localDay();
    if (d < 0) return -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    float sum = daySum(d + off);
    xSemaphoreGive(s_mx);
    float k = kNow(nullptr);
    return sum < 0 || k <= 0 ? -1 : sum * k;
}

int weatherNowRad() {
    int32_t d = localDay();
    if (d < 0) return -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int r = -1;
    int32_t i = (d - s_fcDay) * 24 + localHour();
    if (s_have && i >= 0 && i < 48) r = s_rad[i];
    xSemaphoreGive(s_mx);
    return r;
}

// ------------------------------------------------------------------ обучение

static void kSave() {
    Preferences p;
    if (!p.begin("wx", false)) return;
    p.putBytes("k", s_k, sizeof(float) * s_kn);
    p.putInt("kday", s_kDay);
    p.end();
}

// Вечером, когда солнце уже село: факт дня / радиация дня.
static void learn(int32_t day, int hour) {
    if (hour < 21 || s_kDay == day) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    float sum = daySum(day);
    xSemaphoreGive(s_mx);
    if (sum < 0) return;                        // прогноза на сегодня нет — подождём
    s_kDay = day;
    InvState v = invSnapshot();
    float pv, g, l;
    statsToday(pv, g, l);
    float kwh = v.pvKwhToday >= 0 ? v.pvKwhToday : pv / 1000;
    if (sum >= 0.5f && kwh >= 0.05f) {          // совсем тёмный день ничему не учит
        memmove(&s_k[1], &s_k[0], sizeof(float) * (NK - 1));
        s_k[0] = kwh / sum;
        if (s_kn < NK) s_kn++;
        Serial.printf("[wx] коэффициент дня %.2f (%.2f кВт·ч / %.2f кВт·ч/м²)\n", s_k[0], kwh, sum);
    }
    kSave();
}

// ------------------------------------------------------------------ автоматика

static void autoSave() {
    Preferences p;
    if (!p.begin("wx", false)) return;
    p.putBool("act", s_active);
    p.putInt("dec", s_decided);
    p.end();
}

static bool inWindow(int h) {
    int f = cfg().autoFrom, t = cfg().autoTo;
    if (f == t) return false;
    return f > t ? (h >= f || h < t) : (h >= f && h < t);
}

static void want(int8_t chp) { s_want = chp; s_wantMs = millis() - 60001; }

static void autoStep(int32_t day, int hour) {
    InvState v = invSnapshot();
    BmsState b = stateSnapshot();
    // Добиться нужного CHP: переключение может быть занято или не встать —
    // повторяем раз в минуту, пока инвертор не покажет то, что нужно.
    if (s_want >= 0 && v.fresh) {
        if (v.prioChg == s_want) s_want = -1;
        else if (millis() - s_wantMs > 60000) { s_wantMs = millis(); invSetSetting(1, s_want); }
    }
    if (!cfg().autoOn) {
        if (s_active) {                          // выключили посреди ночи — вернуть как было
            s_active = false; autoSave(); want(0);
            s_msg = "автоматику выключили — CHP возвращён на PV";
            tgNotify("☀️ Автоматику зарядки выключили — возвращаю CHP на PV.");
        }
        return;
    }
    bool win = inWindow(hour);
    int f = cfg().autoFrom, t = cfg().autoTo;
    int32_t night = (f > t && hour < t) ? day - 1 : day;     // ночь называется днём, в который началась
    int8_t  off = (f > t ? night + 1 : night) - day;         // день, чей прогноз решает: 0 — сегодня, 1 — завтра

    if (win && s_decided != night) {
        s_decided = night;
        autoSave();
        float fc = weatherPvKwh(off);
        String fcs = String(fc, 1) + " кВт·ч";
        if (fc < 0) {
            s_msg = "прогноза нет — ночную зарядку не включал";
            tgNotify("🌫 Прогноза выработки нет — ночную зарядку от сети не включаю. Проверьте координаты в настройках.");
        } else if (fc >= cfg().autoBelow) {
            s_msg = "по прогнозу " + fcs + " — от сети не заряжал";
        } else if (!v.fresh || v.prioChg != 0) {
            s_msg = "по прогнозу " + fcs + ", но CHP не PV — не трогал";
            tgNotify("🌥 Завтра пасмурно (" + fcs + "), но CHP сейчас " +
                     String(v.prioChg == 1 ? "PNG" : v.prioChg == 2 ? "OPV" : "неизвестен") +
                     ", а не PV — ничего не меняю.");
        } else {
            s_active = true; autoSave(); want(1);
            s_msg = "по прогнозу " + fcs + " — заряжал от сети";
            tgNotify("🌥 Завтра пасмурно: по прогнозу " + fcs + " (порог " + String(cfg().autoBelow, 1) +
                     "). Ночью заряжаю батарею от сети: CHP → PNG, до " + String(t) + ":00 или до " +
                     String(cfg().autoSoc) + " %.");
        }
    }
    if (s_active && s_want < 0 && v.fresh && v.prioChg != 1) {   // CHP поменяли руками
        s_active = false; autoSave();
        s_msg = "CHP поменяли вручную — до следующей ночи не вмешиваюсь";
        tgNotify("ℹ️ CHP поменяли вручную — автоматика до следующей ночи не вмешивается.");
    }
    bool full = b.socKnown && b.soc >= cfg().autoSoc;
    if (s_active && (!win || full)) {
        s_active = false; autoSave(); want(0);
        String soc = b.socKnown ? String(b.soc, 0) + " %" : String("неизвестен");
        s_msg += full ? ", зарядил до " + soc : ", окно кончилось на " + soc;
        tgNotify("☀️ Ночная зарядка от сети закончена, заряд " + soc + ". CHP → PV.");
    }
}

// ------------------------------------------------------------------ loop

void weatherLoop() {
    static uint32_t last = 0;
    if (millis() - last < 10000) return;
    last = millis();
    int32_t day = localDay();
    if (day < 0) return;
    int hour = localHour();
    float pv0 = weatherPvKwh(0);
    if (pv0 >= 0) statsSetPvForecast(pv0 * 1000);
    learn(day, hour);
    autoStep(day, hour);
}

void weatherStart() {
    s_tls = xSemaphoreCreateMutex();
    s_mx = xSemaphoreCreateMutex();
    for (int i = 0; i < 48; i++) s_rad[i] = -1;
    Preferences p;
    if (p.begin("wx", true)) {
        size_t n = p.getBytesLength("k") / sizeof(float);
        if (n > NK) n = NK;
        if (n) p.getBytes("k", s_k, n * sizeof(float));
        s_kn = n;
        s_kDay = p.getInt("kday", -1);
        s_active = p.getBool("act", false);
        s_decided = p.getInt("dec", -1);
        p.end();
    }
    xTaskCreatePinnedToCore(task, "wx", 8192, nullptr, 1, nullptr, 1);
}

// ------------------------------------------------------------------ выдача

static String esc(String s) { s.replace("\"", "'"); return s; }

String weatherJson() {
    String src;
    float k = kNow(&src);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int32_t d = localDay();
    float s0 = daySum(d), s1 = daySum(d + 1);
    bool have = s_have;
    uint32_t age = have ? (millis() - s_fetchedMs) / 1000 : 0;
    String err = s_err;
    xSemaphoreGive(s_mx);
    const Config& c = cfg();
    String j = "{\"set\":" + String(c.lat != 0 || c.lon != 0 ? "true" : "false");
    j += ",\"lat\":" + String(c.lat, 4) + ",\"lon\":" + String(c.lon, 4);
    j += ",\"kwp\":" + String(c.pvKwp, 2) + ",\"price\":" + String(c.price, 2);
    j += ",\"have\":" + String(have ? "true" : "false") + ",\"age\":" + String(age);
    j += ",\"err\":\"" + esc(err) + "\"";
    j += ",\"rad0\":" + String(s0, 2) + ",\"rad1\":" + String(s1, 2);
    j += ",\"pv0\":" + String(s0 >= 0 && k > 0 ? s0 * k : -1, 1) + ",\"pv1\":" + String(s1 >= 0 && k > 0 ? s1 * k : -1, 1);
    j += ",\"now\":" + String(weatherNowRad());
    j += ",\"k\":" + String(k, 2) + ",\"kSrc\":\"" + esc(src) + "\",\"kN\":" + String(s_kn);
    j += ",\"auto\":{\"on\":" + String(c.autoOn ? "true" : "false") + ",\"below\":" + String(c.autoBelow, 1) +
         ",\"from\":" + String(c.autoFrom) + ",\"to\":" + String(c.autoTo) + ",\"soc\":" + String(c.autoSoc) +
         ",\"active\":" + String(s_active ? "true" : "false") + ",\"msg\":\"" + esc(s_msg) + "\"}}";
    return j;
}

String weatherText() {
    const Config& c = cfg();
    if (c.lat == 0 && c.lon == 0) return "🌤 Погода не настроена: задайте координаты на странице модуля, «Настройки» → «Авто».";
    float p0 = weatherPvKwh(0), p1 = weatherPvKwh(1);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int32_t d = localDay();
    float s0 = daySum(d), s1 = daySum(d + 1);
    String err = s_err;
    xSemaphoreGive(s_mx);
    if (s0 < 0 && s1 < 0) return "🌤 Прогноза пока нет" + (err.length() ? ": " + err : String("."));
    String src;
    kNow(&src);
    String r = "🌤 Прогноз солнца\n";
    r += "сегодня: " + (p0 >= 0 ? String(p0, 1) + " кВт·ч" : String("—")) + " (радиация " + String(s0, 1) + " кВт·ч/м²)\n";
    r += "завтра: " + (p1 >= 0 ? String(p1, 1) + " кВт·ч" : String("—")) + " (радиация " + String(s1, 1) + " кВт·ч/м²)";
    if (src.length()) r += "\nкоэффициент панелей — " + src;
    else r += "\nв кВт·ч прогноза пока нет: укажите мощность панелей или подождите 3 дня — модуль научится по факту";
    if (c.autoOn) r += "\nночная зарядка: включена, порог " + String(c.autoBelow, 1) + " кВт·ч" +
                       (s_active ? ", сейчас заряжаю от сети" : "") + (s_msg.length() ? "\nпоследнее решение: " + s_msg : String(""));
    return r;
}
