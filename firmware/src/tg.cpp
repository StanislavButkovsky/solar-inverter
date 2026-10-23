#include "tg.h"
#include "tg_roots.h"
#include "cfg.h"
#include "state.h"
#include "stats.h"
#include "ota.h"
#include "profile.h"
#include "bootlog.h"
#include "weather.h"
#include "inv_codes.h"
#include "schedule.h"
#include "battlife.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

static const uint32_t LINK_LOST_MS = 5UL * 60 * 1000;
static const uint32_t POLL_MS = 15000;           // команды доходят за 15 с

static QueueHandle_t s_q = nullptr;              // char* из strdup, освобождает задача
static volatile bool s_testReq = false;
static String   s_bot;                           // @имя бота после getMe
static String   s_err;                           // последняя ошибка — на страницу
static String   s_code;                          // код привязки, пока чата нет
static int64_t  s_offset = 0;                    // следующий update_id

// Токен и чат меняются со страницы (задача веба), а читает их задача бота.
// String при перезаписи переезжает в памяти — без мьютекса чтение может
// поймать освобождённый буфер. Поэтому только копии и только под ним.
static SemaphoreHandle_t s_mx = nullptr;
static String token() { xSemaphoreTake(s_mx, portMAX_DELAY); String t = cfg().tgToken; xSemaphoreGive(s_mx); return t; }
static String chatId() { xSemaphoreTake(s_mx, portMAX_DELAY); String c = cfg().tgChat; xSemaphoreGive(s_mx); return c; }
static void setChat(const String& c) { xSemaphoreTake(s_mx, portMAX_DELAY); cfg().tgChat = c; cfgSave(); xSemaphoreGive(s_mx); }

// ------------------------------------------------------------------ текст

static String hhmm(time_t t) {
    if (t < 1700000000) return "—";
    time_t loc = t + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
    return b;
}

static String dur(uint32_t s) {
    char b[32];
    if (s >= 3600) snprintf(b, sizeof(b), "%lu ч %lu мин", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else if (s >= 60) snprintf(b, sizeof(b), "%lu мин", (unsigned long)(s / 60));
    else snprintf(b, sizeof(b), "%lu с", (unsigned long)s);
    return b;
}

static String battLine() {
    BmsState b = stateSnapshot();
    if (!b.fresh) return "батарея: нет данных";
    String r = "батарея " + String(b.voltage, 1) + " В";
    if (b.socKnown) r += ", " + String(b.soc, 0) + " %";
    if (fabsf(b.current) > 0.5f) r += ", " + String(b.current, 1) + " А";
    float avg = statsBattAvgA();
    uint32_t eta = b.socKnown ? battEtaS(b.soc, b.capAh, avg, cfg().tgSocLow) : 0;
    if (eta) r += avg < 0 ? "\nхватит примерно на " + dur(eta) + " (до " + String(cfg().tgSocLow) + " %)"
                          : "\nдо полного примерно " + dur(eta);
    return r;
}

static const char* modeName(uint8_t m) {
    switch (m) {
    case INVM_LINE:    return "от сети";
    case INVM_BATTERY: return "от батареи и солнца";
    case INVM_BYPASS:  return "обход";
    case INVM_FAULT:   return "АВАРИЯ";
    case INVM_STANDBY: return "ожидание";
    default:           return "нет связи";
    }
}

static String statusText() {
    InvState v = invSnapshot();
    String r = "☀️ Состояние\n";
    if (!v.fresh) {
        r += "инвертор: нет связи\n";
    } else {
        r += "режим: " + String(modeName(v.mode)) + "\n";
        r += "сеть: " + (v.gridOn ? String(v.gridV, 0) + " В" : String("НЕТ")) + "\n";
        r += "нагрузка: " + String(v.outW, 0) + " Вт (" + String(v.loadPct) + " %)\n";
        r += "панели: " + String(v.pvW, 0) + " Вт\n";
        r += "температура: " + String(v.tempC) + " °C\n";
    }
    r += battLine() + "\n";
    float pv, grid, load;
    statsToday(pv, grid, load);
    if (v.pvKwhToday >= 0) { pv = v.pvKwhToday * 1000; grid = v.gridKwhToday * 1000; load = v.loadKwhToday * 1000; }
    r += "за сегодня: солнце " + String(pv / 1000, 2) + ", сеть " + String(grid / 1000, 2) +
         ", нагрузка " + String(load / 1000, 2) + " кВт·ч";
    return r;
}

static String todayText() {
    InvState v = invSnapshot();
    float pv, grid, load;
    statsToday(pv, grid, load);
    if (v.pvKwhToday >= 0) { pv = v.pvKwhToday * 1000; grid = v.gridKwhToday * 1000; load = v.loadKwhToday * 1000; }
    uint32_t outS; uint16_t outN;
    statsTodayOutages(outS, outN);
    String r = "📊 За сегодня\n";
    r += "солнце: " + String(pv / 1000, 2) + " кВт·ч\n";
    r += "из сети: " + String(grid / 1000, 2) + " кВт·ч\n";
    r += "нагрузка: " + String(load / 1000, 2) + " кВт·ч\n";
    r += "без сети: " + (outN ? dur(outS) + ", " + String(outN) + " раз" : String("не было")) + "\n";
    float fc0 = weatherPvKwh(0), fc1 = weatherPvKwh(1);
    if (fc0 > 0) r += "прогноз солнца был " + String(fc0, 1) + " кВт·ч — факт " + String(pv / 1000 / fc0 * 100, 0) + " %\n";
    if (fc1 >= 0) r += "завтра по прогнозу: " + String(fc1, 1) + " кВт·ч\n";
    if (cfg().price > 0) r += "солнце сэкономило ≈ " + String(pv / 1000 * cfg().price, 0) + " ₽\n";
    r += battLine();
    return r;
}

// ------------------------------------------------------------------ сводки за период

static String dm(uint32_t day) {
    time_t t = (time_t)day * 86400;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char b[8];
    snprintf(b, sizeof(b), "%02d.%02d", tmv.tm_mday, tmv.tm_mon + 1);
    return b;
}

static uint8_t mday(uint32_t day) {
    time_t t = (time_t)day * 86400;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return tmv.tm_mday;
}

static const char* const MONTH[] = { "январь", "февраль", "март", "апрель", "май", "июнь", "июль",
                                     "август", "сентябрь", "октябрь", "ноябрь", "декабрь" };

static String kwh(float wh) { String r = String(wh / 1000, wh >= 100000 ? 0 : 1); r.replace('.', ','); return r + " кВт·ч"; }

// «(прошлая неделя 18,0 — +18 %)»; меньше 0,1 кВт·ч в прошлом — сравнивать не с чем.
static String vs(float now, float before, const char* what) {
    if (before < 100) return "";
    int pct = (int)lroundf((now - before) / before * 100);
    return String(" (") + what + " " + kwh(before) + ", " + (pct > 0 ? "+" : pct < 0 ? "−" : "±") + String(abs(pct)) + " %)";
}

// Сводка за дни from..to против prevFrom..prevTo. Только из loop().
static String periodText(const String& title, uint32_t from, uint32_t to, uint32_t pFrom, uint32_t pTo, const char* prevName) {
    StatSum a, b;
    if (!statsSum(from, to, a)) return "Сводки нет: у модуля нет точного времени.";
    bool havePrev = statsSum(pFrom, pTo, b) && b.dataS > 0;
    if (!havePrev) memset(&b, 0, sizeof(b));
    uint32_t days = to - from + 1;
    if (!a.dataS) return title + "\nданных за этот период нет — модуль тогда не работал или статистику стирали.";
    String r = title + "\n";
    r += "солнце: " + kwh(a.pvWh) + vs(a.pvWh, b.pvWh, prevName) + "\n";
    r += "из сети: " + kwh(a.gridWh) + vs(a.gridWh, b.gridWh, prevName) + "\n";
    r += "нагрузка: " + kwh(a.loadWh) + vs(a.loadWh, b.loadWh, prevName) + "\n";
    if (a.pvFcWh > 0) r += "прогноз солнца: " + kwh(a.pvFcWh) + " — факт " + String((int)lroundf(a.pvWh / a.pvFcWh * 100)) + " %\n";
    if (cfg().price > 0) {
        r += "экономия: ≈ " + String((int)lroundf(a.pvWh / 1000 * cfg().price)) + " ₽";
        if (havePrev && b.pvWh > 0) r += " (" + String(prevName) + " " + String((int)lroundf(b.pvWh / 1000 * cfg().price)) + " ₽)";
        r += "\n";
    }
    r += "без сети: " + (a.outages ? dur(a.outageS) + ", " + String(a.outages) + " раз" : String("не было"));
    if (havePrev) r += String(" (") + prevName + ": " + (b.outages ? dur(b.outageS) + ", " + String(b.outages) + " раз" : String("не было")) + ")";
    r += "\n";
    // Модуль мог не работать часть периода — тогда цифры неполные, и это надо видеть.
    float cover = a.dataS / 86400.0f;
    if (cover < days - 0.5f) {
        String c = String(cover, 1);
        c.replace('.', ',');
        r += "⚠️ данные есть за " + c + " из " + String(days) + " сут — цифры неполные\n";
    }
    if (a.pvFcWh <= 0 && cfg().lat == 0 && cfg().lon == 0) r += "(прогноз появится, когда в «Авто» будут координаты)\n";
    r.trim();
    return r;
}

static String weekText() {
    uint32_t d = statsDay();
    if (!d) return "Сводки нет: у модуля нет точного времени.";
    return periodText("📅 Неделя " + dm(d - 6) + "–" + dm(d), d - 6, d, d - 13, d - 7, "прошлая неделя");
}

// full: за прошлый календарный месяц целиком (сводка 1-го числа);
// иначе — с начала этого месяца против тех же дней прошлого.
static String monthText(bool full) {
    uint32_t d = statsDay();
    if (!d) return "Сводки нет: у модуля нет точного времени.";
    uint32_t first = d - (mday(d) - 1);
    if (full) {
        uint32_t to = first - 1, from = to - (mday(to) - 1);
        uint32_t pTo = from - 1, pFrom = pTo - (mday(pTo) - 1);
        time_t t = (time_t)from * 86400;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        return periodText("🗓 Итоги месяца: " + String(MONTH[tmv.tm_mon]) + " " + String(tmv.tm_year + 1900),
                          from, to, pFrom, pTo, "месяцем раньше");
    }
    uint32_t n = d - first;                              // сколько дней прошло, кроме сегодня
    uint32_t pFirst = (first - 1) - (mday(first - 1) - 1);
    uint32_t pTo = pFirst + n < first - 1 ? pFirst + n : first - 1;
    return periodText("🗓 Месяц " + dm(first) + "–" + dm(d), first, d, pFirst, pTo, "те же дни прошлого месяца");
}

// Команды бота приходят в его задаче, а файлы статистики читает только
// loop(): запрос — флагом, текст собирает tgLoop().
static volatile uint8_t s_reportReq = 0;              // 1 — неделя, 2 — месяц, 3 — батарея

String tgReport(uint8_t what) { return what == 1 ? weekText() : what == 2 ? monthText(false) : monthText(true); }

static void reportLoop() {
    uint8_t r = s_reportReq;
    if (!r) return;
    s_reportReq = 0;
    tgNotify(r == 1 ? weekText() : r == 2 ? monthText(false) : battText());
}

// Последние отключения — из того же журнала, что на странице.
static String outagesText() {
    String j = outagesJson(5);
    String r = "⚡ Последние отключения\n";
    int n = 0, pos = 0;
    while ((pos = j.indexOf("\"s\":", pos)) >= 0) {
        uint32_t s = strtoul(j.c_str() + pos + 4, nullptr, 10);
        int pe = j.indexOf("\"e\":", pos);
        uint32_t e = pe >= 0 ? strtoul(j.c_str() + pe + 4, nullptr, 10) : s;
        bool open = j.indexOf("\"open\":true", pos) == j.indexOf("\"open\":", pos);
        time_t loc = (time_t)s + 3 * 3600;
        struct tm lt;
        gmtime_r(&loc, &lt);
        char d[16];
        snprintf(d, sizeof(d), "%02d.%02d ", lt.tm_mday, lt.tm_mon + 1);
        r += String(d) + hhmm(s) + "–" + (open ? String("идёт") : hhmm(e)) + ", " + dur(e - s) + "\n";
        n++;
        pos += 4;
    }
    return n ? r : String("⚡ Отключений сети не было");
}

static const char* HELP =
    "Команды:\n/status — что сейчас\n/today — итоги дня\n/week — за 7 дней\n/month — с начала месяца\n"
    "/outages — последние отключения сети\n"
    "/opp — приоритет работы (посмотреть, переключить)\n/chp — приоритет зарядки\n"
    "/settings — все настройки инвертора\n/schedule — расписание режимов\n/battery — износ батареи\n"
    "/forecast — прогноз солнца и ночная зарядка\n/csv — вся статистика таблицей\n/boots — перезапуски модуля\n/help — этот список";

// ------------------------------------------------------------------ очередь

void tgNotify(const String& text) {
    if (!s_q || !s_mx || token().isEmpty() || chatId().isEmpty()) return;
    char* p = strdup(text.c_str());
    if (p && xQueueSend(s_q, &p, 0) != pdTRUE) free(p);
}

void tgOutage(bool started, uint32_t seconds) {
    time_t now = time(nullptr);
    if (started)
        tgNotify("⚡ Сеть пропала в " + hhmm(now - seconds) + "\n" + battLine());
    else
        tgNotify("✅ Сеть вернулась в " + hhmm(now) + ", её не было " + dur(seconds) + "\n" + battLine());
}

// ------------------------------------------------------------------ HTTPS

static String urlenc(const String& s) {
    static const char* hex = "0123456789ABCDEF";
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Запрос к API; тело ответа — в out. false — сеть, TLS или ok:false.
static bool api(WiFiClientSecure& cli, const String& method, const String& body, String& out, uint16_t timeoutS = 15) {
    HTTPClient http;
    http.setTimeout(timeoutS * 1000);
    http.setConnectTimeout(8000);
    if (!http.begin(cli, "https://api.telegram.org/bot" + token() + "/" + method)) { s_err = "не удалось начать запрос"; return false; }
    int code;
    if (body.length()) {
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(body);
    } else {
        code = http.GET();
    }
    out = code > 0 ? http.getString() : String();
    http.end();
    if (code <= 0) { s_err = "нет связи с api.telegram.org (" + HTTPClient::errorToString(code) + ")"; return false; }
    if (code == 401 || code == 404) { s_err = "токен не подходит"; return false; }
    if (out.indexOf("\"ok\":true") < 0) { s_err = "ответ " + String(code); return false; }
    s_err = "";
    return true;
}

static void send(WiFiClientSecure& cli, const String& chat, const String& text) {
    String out;
    api(cli, "sendMessage", "chat_id=" + chat + "&text=" + urlenc(text) + "&disable_web_page_preview=1", out);
}

// Таблица статистики файлом. Собирает её loop() — там живёт статистика и её
// файлы, — а отправляет задача бота: s_csvReq → loop() → s_csv → задача.
// Строка на 20–30 КБ уходит в PSRAM.
static volatile bool s_csvReq = false;
static String* volatile s_csv = nullptr;
static String s_csvCaption;

static void csvBuild() {
    if (!s_csvReq || s_csv) return;
    s_csvReq = false;
    String* c = new String();
    c->reserve(32768);
    statsCsv([](const String& part, void* ctx) { *(String*)ctx += part; }, c);
    s_csv = c;
}

static void sendCsv(WiFiClientSecure& cli, const String& chat) {
    String* csv = s_csv;
    if (!csv) return;
    time_t loc = time(nullptr) + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    char name[40];
    snprintf(name, sizeof(name), "solar-%04d-%02d-%02d.csv", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    String bnd = "----solar" + String(esp_random(), HEX);
    String body;
    body.reserve(csv->length() + 600);
    body = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chat + "\r\n";
    body += "--" + bnd + "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" + s_csvCaption + "\r\n";
    body += "--" + bnd + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" + name +
            "\"\r\nContent-Type: text/csv\r\n\r\n";
    body += *csv;
    body += "\r\n--" + bnd + "--\r\n";
    s_csv = nullptr;
    delete csv;
    HTTPClient http;
    http.setTimeout(30000);
    http.setConnectTimeout(8000);
    if (!http.begin(cli, "https://api.telegram.org/bot" + token() + "/sendDocument")) return;
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + bnd);
    int code = http.POST((uint8_t*)body.c_str(), body.length());
    http.end();
    if (code != 200) s_err = "таблица не ушла: ответ " + String(code);
}

// Строковое поле после ключа: "key":"значение" — без разбора экранирования,
// для команд и имени бота его хватает.
static String field(const String& j, const char* key, int from) {
    String k = String("\"") + key + "\":\"";
    int p = j.indexOf(k, from);
    if (p < 0) return "";
    p += k.length();
    int e = j.indexOf('"', p);
    return e < 0 ? String("") : j.substring(p, e);
}

// Проверка записи OPP с телефона: человек стоит у инвертора, шлёт /opptest,
// модуль сам пишет PU, потом PBG и раз в секунду записывает, что инвертор
// держит, — с экраном сверяется глазами. Им 21.10.2026 запись и сверили.
static volatile bool s_oppReq = false;
static const char* const OPP_N[] = { "GRD", "PU", "PBG" };

static String oppNow(const InvState& v) {
    String o = v.prioOut < 3 ? OPP_N[v.prioOut] : "?";
    const char* m = v.mode == INVM_LINE ? "сеть" : v.mode == INVM_BATTERY ? "батарея" :
                    v.mode == INVM_BYPASS ? "обход" : v.mode == INVM_STANDBY ? "ожидание" : "?";
    return o + "/" + m;
}

static void oppTestLoop() {
    // шаги: 0 нет теста; 1 пауза перед PU; 2 смотрим PU; 3 пауза перед PBG; 4 смотрим PBG
    static uint8_t step = 0;
    static uint32_t t0 = 0, lastS = 0;
    static String log, prev;
    if (s_oppReq && step == 0) {
        s_oppReq = false;
        InvState v = invSnapshot();
        step = 1; t0 = millis();
        tgNotify("🧪 Тест OPP. Сейчас: " + oppNow(v) + "\nЧерез 30 с ставлю PU — смотрите на экран инвертора.");
        return;
    }
    s_oppReq = false;
    if (step == 0) return;
    uint32_t el = millis() - t0;
    if (step == 1 || step == 3) {
        if (el < 30000) return;
        uint8_t code = step == 1 ? 1 : 2;
        String e = invSetSetting(0, code);
        if (!e.isEmpty()) { tgNotify("🧪 Тест OPP прерван: " + e); step = 0; return; }
        step++; t0 = millis(); lastS = 0; prev = ""; log = "";
        tgNotify(String("🧪 Записал ") + OPP_N[code] + ". Смотрю 60 с, раз в секунду.");
        return;
    }
    if (millis() - lastS >= 1000) {                      // шаги 2 и 4
        lastS = millis();
        String n = oppNow(invSnapshot());
        if (n != prev) { log += "\n+" + String(el / 1000) + " с: " + n; prev = n; }
    }
    if (el < 60000) return;
    const char* what = step == 2 ? "PU" : "PBG";
    tgNotify(String("🧪 После записи ") + what + " (OPP/работа):" + log);
    if (step == 2) {
        step = 3; t0 = millis();
        tgNotify("🧪 Через 30 с ставлю PBG — смотрите на экран.");
    } else {
        step = 0;
        tgNotify("🧪 Тест OPP закончен. Запомните, что показывал экран после PU и после PBG. "
                 "Если сейчас не PBG — поставьте с панели.");
    }
}

// Переключение режимов из бота: /opp pbg → «точно?» → /yes в течение 2 минут.
// Подтверждение — потому что одна опечатка в чате не должна переводить дом
// на сеть. Само переключение — в loop() (tgLoop), там же, где его делает
// страница; итог приходит обычным «⚙️ OPP → PBG: встало».
static const char* const SET_N[2][3] = { { "GRD", "PU", "PBG" }, { "PV", "PNG", "OPV" } };
static const char* const SET_D[2][3] = {
    { "сеть в приоритете", "солнце в приоритете", "солнце → батарея → сеть" },
    { "заряд от солнца в приоритете", "заряд от солнца и сети", "заряд только от солнца" } };
static int8_t   s_askWhat = -1, s_askCode = -1;      // ждёт /yes
static uint32_t s_askMs = 0;
static volatile int8_t s_doWhat = -1, s_doCode = -1; // подтверждено — выполнить в loop()

static String setMenu(uint8_t what) {
    InvState v = invSnapshot();
    uint8_t now = what ? v.prioChg : v.prioOut;
    String r = String(what ? "CHP — приоритет зарядки" : "OPP — приоритет работы") + ", сейчас " +
               (now < 3 ? SET_N[what][now] : "неизвестно") + ".\nВарианты:";
    for (uint8_t i = 0; i < 3; i++) {
        String c = SET_N[what][i];
        c.toLowerCase();
        r += String("\n/") + (what ? "chp_" : "opp_") + c + " — " + SET_D[what][i];
    }
    return r;
}

static String setAsk(uint8_t what, String arg) {
    arg.trim();
    arg.toUpperCase();
    if (!arg.length()) return setMenu(what);
    int8_t code = -1;
    for (uint8_t i = 0; i < 3; i++) if (arg == SET_N[what][i]) code = i;
    if (code < 0) return "Такого варианта нет.\n\n" + setMenu(what);
    InvState v = invSnapshot();
    if (!v.fresh) return "Инвертор сейчас не на связи — переключить не получится.";
    uint8_t now = what ? v.prioChg : v.prioOut;
    if (now == code) return String("Уже стоит ") + SET_N[what][code] + ".";
    s_askWhat = what; s_askCode = code; s_askMs = millis();
    return String("Переключить ") + (what ? "CHP" : "OPP") + ": " + (now < 3 ? SET_N[what][now] : "?") + " → " +
           SET_N[what][code] + " (" + SET_D[what][code] + ")?\nНажмите /yes в течение 2 минут.";
}

static String setConfirm() {
    if (s_askWhat < 0 || millis() - s_askMs > 120000) { s_askWhat = -1; return "Подтверждать нечего — сначала /opp или /chp."; }
    s_doCode = s_askCode;
    s_doWhat = s_askWhat;
    s_askWhat = -1;
    return "Отправляю инвертору, итог пришлю.";
}

static void setLoop() {
    if (s_doWhat < 0) return;
    uint8_t what = s_doWhat, code = s_doCode;
    s_doWhat = -1;
    String e = invSetSetting(what, code);
    if (e.length()) tgNotify("⚙️ Не переключил: " + e);
}

static void handle(WiFiClientSecure& cli, const String& chat, String text) {
    text.trim();
    int sp = text.indexOf(' ');
    String cmd = sp < 0 ? text : text.substring(0, sp);
    String arg = sp < 0 ? String("") : text.substring(sp + 1);
    int at = cmd.indexOf('@');
    if (at > 0) cmd = cmd.substring(0, at);
    cmd.toLowerCase();
    // /opp_pbg — то же, что «/opp pbg», но нажимается в сообщении бота целиком.
    if ((cmd.startsWith("/opp_") || cmd.startsWith("/chp_")) && cmd.length() > 5) {
        arg = cmd.substring(5);
        cmd = cmd.substring(0, 4);
    }

    String bound = chatId();
    if (bound.isEmpty()) {                               // привязки ещё нет
        arg.trim();
        if (cmd == "/start" && arg == s_code) {
            setChat(chat);
            send(cli, chat, String("Привязано. Буду писать сюда.\n\n") + HELP);
        } else {
            send(cli, chat, "Чтобы привязать бота, отправьте /start и код со страницы модуля.");
        }
        return;
    }
    if (chat != bound) return;                           // чужие чаты — молча мимо

    if (cmd == "/status" || cmd == "/start") send(cli, chat, statusText());
    else if (cmd == "/today")   send(cli, chat, todayText());
    else if (cmd == "/outages") send(cli, chat, outagesText());
    else if (cmd == "/boots")   send(cli, chat, "🔁 " + bootText());
    else if (cmd == "/settings") send(cli, chat, invSettingsText() + "\nМенять — на странице модуля: «Настройки» → «Инвертор».");
    else if (cmd == "/schedule") send(cli, chat, schedText());
    else if (cmd == "/week")    s_reportReq = 1;
    else if (cmd == "/month")   s_reportReq = 2;
    else if (cmd == "/battery") s_reportReq = 3;
    else if (cmd == "/opp")     send(cli, chat, setAsk(0, arg));
    else if (cmd == "/chp")     send(cli, chat, setAsk(1, arg));
    else if (cmd == "/yes")     send(cli, chat, setConfirm());
    else if (cmd == "/forecast" || cmd == "/weather") send(cli, chat, weatherText());
    else if (cmd == "/csv") { s_csvCaption = "📎 Статистика по дням — открывается в Excel"; s_csvReq = true; send(cli, chat, "Собираю таблицу, сейчас пришлю."); }
    else if (cmd == "/opptest") { s_oppReq = true; send(cli, chat, "Тест OPP принят — начну в течение пары секунд."); }
    else                        send(cli, chat, HELP);
}

static void poll(WiFiClientSecure& cli) {
    String out;
    if (!api(cli, "getUpdates", "offset=" + String((long long)s_offset) + "&timeout=0&allowed_updates=%5B%22message%22%5D", out, 20)) return;
    int pos = 0;
    while ((pos = out.indexOf("\"update_id\":", pos)) >= 0) {
        int64_t id = atoll(out.c_str() + pos + 12);
        int next = out.indexOf("\"update_id\":", pos + 12);
        String upd = out.substring(pos, next < 0 ? out.length() : next);
        s_offset = id + 1;
        int c = upd.indexOf("\"chat\":{\"id\":");
        String text = field(upd, "text", 0);
        if (c >= 0 && text.length()) {
            String chat = String(atoll(upd.c_str() + c + 13));
            handle(cli, chat, text);
        }
        pos += 12;
    }
}

static void task(void*) {
    WiFiClientSecure cli;
    cli.setCACert(TG_ROOT_CA);
    bool greeted = false;
    for (;;) {
        if (token().isEmpty() || WiFi.status() != WL_CONNECTED || otaBusy()) {   // при обновлении — молчим
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        tlsLock();                               // TLS один за раз — с погодой делим
        if (!s_bot.length()) {
            String out;
            if (api(cli, "getMe", "", out)) s_bot = "@" + field(out, "username", 0);
            else { cli.stop(); tlsUnlock(); vTaskDelay(pdMS_TO_TICKS(15000)); continue; }
        }
        String chat = chatId();
        if (chat.length() && !greeted) {
            greeted = true;
            send(cli, chat, String(bootWasBad() ? "⚠️" : "🟢") + " Модуль на связи, сборка " + fwBuild() +
                            "\nперезапуск: " + bootReason());
        }
        if (s_testReq) { s_testReq = false; if (chat.length()) send(cli, chat, "Проверка: бот работает.\n\n" + statusText()); }
        if (chat.length() && s_csv) sendCsv(cli, chat);
        char* msg;
        while (chat.length() && xQueueReceive(s_q, &msg, 0) == pdTRUE) {
            send(cli, chat, msg);
            free(msg);
        }
        poll(cli);
        // Соединение не держим: TLS занимает десятки килобайт внутренней
        // памяти, а её на плате впритык (21.10.2026 минимум доходил до 752 байт,
        // и обновление прошивки не нашло 4 КБ). Закрыли — память свободна до
        // следующего опроса; пришло уведомление — очередь разбудит раньше.
        cli.stop();
        tlsUnlock();
        uint32_t t0 = millis();
        while (millis() - t0 < POLL_MS && !uxQueueMessagesWaiting(s_q) && !s_testReq && !s_csv) vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ------------------------------------------------------------------ пороги

// Тревога с выдержкой: условие должно продержаться holdS, и снимается она
// только по своему, более мягкому условию — иначе на границе порога бот
// писал бы каждые 5 секунд.
struct Alarm { uint32_t since = 0; bool sent = false; };

static void alarmStep(Alarm& a, bool bad, bool good, uint32_t holdS, const String& onText, const String& offText) {
    if (!a.sent) {
        if (!bad) { a.since = 0; return; }
        if (!a.since) a.since = millis();
        if (millis() - a.since >= holdS * 1000) { a.sent = true; a.since = 0; tgNotify(onText); }
    } else {
        if (!good) { a.since = 0; return; }
        if (!a.since) a.since = millis();
        if (millis() - a.since >= holdS * 1000) { a.sent = false; a.since = 0; if (offText.length()) tgNotify(offText); }
    }
}

// Пороги — с запасом до того, что инвертор делает сам: греется до 75 °C
// (у HS35 обычно 50–60), нагрузка 90 % (дальше он отключает выход),
// сеть 190–250 В (норма 230 В ±10 %, ГОСТ 29322).
static const int16_t TEMP_HOT = 75, TEMP_OK = 65;
static const uint8_t LOAD_HIGH = 90, LOAD_OK = 75;
static const float   GRID_LO = 190, GRID_HI = 250, GRID_OK_LO = 200, GRID_OK_HI = 245;

static void alarmsLoop(const InvState& v, const BmsState& b) {
    static Alarm hot, load, volt, pv;
    if (!v.fresh || millis() < 120000) return;
    // Коды аварии и предупреждения самой BMS (HS35 передаёт их в GBMS).
    static uint16_t pF = 0, pW = 0;
    if (b.fresh && (b.bmsFault != pF || b.bmsWarn != pW)) {
        if (b.bmsFault && b.bmsFault != pF) tgNotify("❗ BMS батареи сообщает об аварии, код " + String(b.bmsFault) + "\n" + battLine());
        else if (b.bmsWarn && b.bmsWarn != pW) tgNotify("⚠️ BMS батареи: предупреждение, код " + String(b.bmsWarn) + "\n" + battLine());
        else if (!b.bmsFault && !b.bmsWarn) tgNotify("✅ BMS батареи: снова в норме");
        pF = b.bmsFault; pW = b.bmsWarn;
    }
    alarmStep(hot, v.tempC >= TEMP_HOT, v.tempC <= TEMP_OK, 60,
              "🌡 Инвертор греется: " + String(v.tempC) + " °C. Проверьте, не закрыта ли вентиляция.",
              "✅ Температура инвертора в норме: " + String(v.tempC) + " °C");
    alarmStep(load, v.loadPct >= LOAD_HIGH, v.loadPct <= LOAD_OK, 20,
              "🔥 Нагрузка " + String(v.loadPct) + " % (" + String(v.outW, 0) +
              " Вт) — близко к пределу, инвертор может отключить выход. Выключите что-нибудь мощное.",
              "✅ Нагрузка снизилась: " + String(v.loadPct) + " %");
    bool vBad = v.gridOn && v.gridV > 50 && (v.gridV < GRID_LO || v.gridV > GRID_HI);
    bool vOk  = !v.gridOn || (v.gridV >= GRID_OK_LO && v.gridV <= GRID_OK_HI);
    alarmStep(volt, vBad, vOk, 60,
              "⚡ Напряжение сети " + String(v.gridV, 0) + " В — вне нормы (" + String(GRID_LO, 0) + "–" +
              String(GRID_HI, 0) + " В). Технике от сети напрямую это вредно.",
              "✅ Напряжение сети в норме: " + String(v.gridV, 0) + " В");
    // Панели: солнце по прогнозу есть (≥ 300 Вт/м² в этот час), а выработки
    // нет полчаса. Без погоды молчим — в пасмурный день это не поломка.
    // Полная батарея при малой нагрузке — инвертор сам придерживает панели,
    // это тоже не поломка.
    int rad = weatherNowRad();
    bool full = b.socKnown && b.soc >= 95;
    bool pvBad = rad >= 300 && v.pvW < 50 && !full;
    bool pvOk  = v.pvW >= 150 || (rad >= 0 && rad < 150);
    alarmStep(pv, pvBad, pvOk, 1800,
              "☀️ Солнце есть (по прогнозу " + String(rad) + " Вт/м²), а панели дают " + String(v.pvW, 0) +
              " Вт уже полчаса. Проверьте автомат панелей, разъёмы, не засыпало ли их снегом.",
              "✅ Панели снова дают ток: " + String(v.pvW, 0) + " Вт");
}

void tgLoop() {
    oppTestLoop();
    setLoop();
    csvBuild();
    reportLoop();
    static uint32_t last = 0, lastFresh = 0;
    static bool lowSent = false, linkSent = false;
    static uint8_t prevFault = 0;
    static int32_t summaryDay = -1;
    if (millis() - last < 5000 || !s_mx || chatId().isEmpty()) return;
    last = millis();

    InvState v = invSnapshot();
    BmsState b = stateSnapshot();

    // Ниже порога должно продержаться 2 минуты — одиночный ложный замер до
    // телефона не доходит; первые 2 минуты после включения не тревожим.
    static uint32_t lowSince = 0;
    bool low = b.fresh && b.socKnown && b.soc < cfg().tgSocLow;
    if (!low) lowSince = 0;
    else if (!lowSince) lowSince = millis();
    if (b.fresh && b.socKnown && millis() > 120000) {
        if (!lowSent && low && millis() - lowSince >= 120000) {
            lowSent = true;
            tgNotify("🔋 Батарея " + String(b.soc, 0) + " % — ниже порога " + String(cfg().tgSocLow) + " %\n" + battLine());
        } else if (lowSent && b.soc > cfg().tgSocLow + 5) {
            lowSent = false;
        }
    }
    alarmsLoop(v, b);
    if (v.fresh) {
        if (v.faultCode != prevFault) {
            String t = invCodeText(v.faultCode);
            if (!v.faultCode)
                tgNotify(prevFault >= 50 ? String("✅ Предупреждение инвертора снято") : String("✅ Авария инвертора снята"));
            else
                tgNotify(String(v.faultCode >= 50 ? "⚠️ Предупреждение инвертора" : "❗ Авария инвертора") + ", код " +
                         String(v.faultCode) + (t.length() ? ": " + t : String("")));
            prevFault = v.faultCode;
        }
        if (linkSent) tgNotify("✅ Связь с инвертором восстановлена");
        linkSent = false;
        lastFresh = millis();
    } else if (lastFresh && !linkSent && millis() - lastFresh > LINK_LOST_MS) {
        linkSent = true;
        tgNotify("⚠️ Нет связи с инвертором больше 5 минут");
    }
    // Сводка в 21:00 по местному времени, раз в сутки.
    if (statsTimeValid()) {
        time_t loc = time(nullptr) + 3 * 3600;
        struct tm lt;
        gmtime_r(&loc, &lt);
        int32_t day = loc / 86400;
        if (lt.tm_hour == 21 && day != summaryDay) {
            summaryDay = day;
            tgNotify(todayText());
            if (lt.tm_wday == 0) tgNotify(weekText());       // воскресенье — ещё и неделя
        }
        if (summaryDay < 0) summaryDay = lt.tm_hour >= 21 ? day : day - 1;   // не слать сводку сразу после включения
        // 1-го числа в 9:00 — вся статистика файлом: бэкап на случай, если плата сломается.
        static int32_t csvMonth = -1;
        int32_t month = (lt.tm_year + 1900) * 12 + lt.tm_mon;
        if (csvMonth < 0 && !(lt.tm_mday == 1 && lt.tm_hour == 9)) csvMonth = month;
        if (lt.tm_mday == 1 && lt.tm_hour == 9 && month != csvMonth) {
            csvMonth = month;
            tgNotify(monthText(true));
            String bm = battMonthText();
            if (bm.length()) tgNotify(bm);
            s_csvCaption = "📎 Статистика за месяц и всё, что было раньше, — бэкап раз в месяц";
            s_csvReq = true;
        }
    }
}

// ------------------------------------------------------------------ настройка

static void newCode() {
    char b[8];
    snprintf(b, sizeof(b), "%06lu", (unsigned long)(esp_random() % 900000 + 100000));
    s_code = b;
}

void tgStart() {
    newCode();
    s_mx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(12, sizeof(char*));
    xTaskCreatePinnedToCore(task, "tg", 12288, nullptr, 2, nullptr, 1);
    String st = token().isEmpty() ? String("не настроен") : (chatId().isEmpty() ? "ждёт /start " + s_code : String("привязан к чату"));
    Serial.printf("[tg] бот %s\n", st.c_str());
}

String tgSetToken(const String& token) {
    String t = token;
    t.trim();
    if (t.length() && (t.indexOf(':') < 0 || t.length() < 30)) return "это не похоже на токен от @BotFather";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    cfg().tgToken = t;
    cfg().tgChat = "";
    cfgSave();
    xSemaphoreGive(s_mx);
    s_bot = "";
    s_err = "";
    s_offset = 0;
    newCode();
    return "";
}

void tgUnbind() {
    setChat("");
    newCode();
}

void tgTest() { s_testReq = true; }

String tgStateJson() {
    bool chat = chatId().length();
    String j = "{\"on\":" + String(token().length() ? "true" : "false");
    j += ",\"bot\":\"" + s_bot + "\"";
    j += ",\"chat\":" + String(chat ? "true" : "false");
    if (!chat) j += ",\"code\":\"" + s_code + "\"";
    j += ",\"low\":" + String(cfg().tgSocLow);
    j += ",\"net\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    String e = s_err;
    e.replace("\"", "'");
    j += ",\"err\":\"" + e + "\"}";
    return j;
}
