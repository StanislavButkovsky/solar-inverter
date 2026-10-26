// Сборки без бота (NO_TG: первое устройство) берут tg_none.cpp.
#if !defined(NO_TG)

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
#include "pace.h"
#include "backup.h"
#include "json.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

static const uint32_t LINK_LOST_MS = 5UL * 60 * 1000;
static const uint32_t POLL_MS = 15000;           // команды доходят за 15 с

// Два канала доставки — Telegram и MAX (с 26.10.2026). У каждого свой
// токен, привязанный чат, код привязки и очередь; тексты, тревоги и команды
// общие. Уведомления уходят во все включённые каналы, ответ на команду — в
// тот, откуда она пришла. Можно включить один любой или оба.
enum : uint8_t { CH_TG = 0, CH_MAX = 1, NCH = 2 };

struct Chan {
    QueueHandle_t q = nullptr;           // char* из strdup, освобождает задача
    String   bot;                        // @имя бота после getMe / me
    String   err;                        // последняя ошибка — на страницу
    String   code;                       // код привязки, пока чата нет
    int64_t  offset = 0;                 // Telegram — следующий update_id, MAX — marker; 0 — ещё нет
    bool     greeted = false;
    volatile bool test = false;
};
static Chan s_ch[NCH];

// Токен и чат меняются со страницы (задача веба), а читает их задача бота.
// String при перезаписи переезжает в памяти — без мьютекса чтение может
// поймать освобождённый буфер. Поэтому только копии и только под ним.
static SemaphoreHandle_t s_mx = nullptr;
static String& tokRef(uint8_t ch)  { return ch == CH_MAX ? cfg().mxToken : cfg().tgToken; }
static String& chatRef(uint8_t ch) { return ch == CH_MAX ? cfg().mxChat : cfg().tgChat; }
static String token(uint8_t ch) { xSemaphoreTake(s_mx, portMAX_DELAY); String t = tokRef(ch); xSemaphoreGive(s_mx); return t; }
static String chatId(uint8_t ch) { xSemaphoreTake(s_mx, portMAX_DELAY); String c = chatRef(ch); xSemaphoreGive(s_mx); return c; }
static void setChat(uint8_t ch, const String& c) { xSemaphoreTake(s_mx, portMAX_DELAY); chatRef(ch) = c; cfgSave(); xSemaphoreGive(s_mx); }
static bool anyChat() { return chatId(CH_TG).length() || chatId(CH_MAX).length(); }
static void notifyCh(uint8_t ch, const String& text);

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

// «сам инвертор: ≈ 51 Вт, ≈ 1,2 кВт·ч в сутки (по 9 ч от батареи)». Считается
// только при работе от батареи (stats.cpp, SelfRec) — меньше получаса такой
// работы за период не показываем: среднее выйдет случайным. days = 0 — сегодня.
static String selfLine(float wh, uint32_t s, uint32_t days = 0) {
    if (!days) statsSelfToday(wh, s);
    if (s < 1800) return "";
    float avgW = wh * 3600 / s;
    String day = String(avgW * 24 / 1000, 1);
    day.replace('.', ',');
    String r = "сам инвертор: ≈ " + String(avgW, 0) + " Вт, ≈ " + day + " кВт·ч в сутки";
    if (days > 1) {
        String all = String(avgW * 24 * days / 1000, 0);
        r += ", за период ≈ " + all + " кВт·ч";
    }
    String t = s >= 3600 ? String(s / 3600) + " ч" : String(s / 60) + " мин";
    return r + " (по " + t + " от батареи)\n";
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
        r += selfLine(0, 0);              // среднее за сегодня: мгновенное слишком шумное
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
    r += selfLine(0, 0);
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
    r += selfLine(a.selfWh, a.selfS, days);
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
static volatile uint8_t s_reportCh = CH_TG;           // кто спросил — туда и ответ

String tgReport(uint8_t what) { return what == 1 ? weekText() : what == 2 ? monthText(false) : monthText(true); }

static void reportLoop() {
    uint8_t r = s_reportReq;
    if (!r) return;
    s_reportReq = 0;
    notifyCh(s_reportCh, r == 1 ? weekText() : r == 2 ? monthText(false) : battText());
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
    "/settings — все настройки инвертора\n/schedule — расписание режимов\n/battery — износ батареи\n/cells — ячейки батарей\n/backup — резервная копия настроек\n"
    "/forecast — прогноз солнца и ночная зарядка\n/csv — вся статистика таблицей\n/boots — перезапуски модуля\n/help — этот список";

// ------------------------------------------------------------------ очередь

static void notifyCh(uint8_t ch, const String& text) {
    if (!s_mx || !s_ch[ch].q || token(ch).isEmpty() || chatId(ch).isEmpty()) return;
    char* p = strdup(text.c_str());
    if (p && xQueueSend(s_ch[ch].q, &p, 0) != pdTRUE) free(p);
}

void tgNotify(const String& text) {
    for (uint8_t ch = 0; ch < NCH; ch++) notifyCh(ch, text);
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
    String& err = s_ch[CH_TG].err;
    if (!http.begin(cli, "https://api.telegram.org/bot" + token(CH_TG) + "/" + method)) { err = "не удалось начать запрос"; return false; }
    int code;
    if (body.length()) {
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(body);
    } else {
        code = http.GET();
    }
    out = code > 0 ? http.getString() : String();
    http.end();
    if (code <= 0) { err = "нет связи с api.telegram.org (" + HTTPClient::errorToString(code) + ")"; return false; }
    if (code == 401 || code == 404) { err = "токен не подходит"; return false; }
    if (out.indexOf("\"ok\":true") < 0) { err = "ответ " + String(code); return false; }
    err = "";
    return true;
}

// MAX: platform-api.max.ru, токен — в заголовке Authorization (в адресе
// MAX его больше не принимает), тело — JSON. 200 — успех. Не platform-api2:
// его сертификат mbedTLS не разбирает (подробно — в tg_roots.h).
static bool maxApi(WiFiClientSecure& cli, const String& path, const String& json, String& out, uint16_t timeoutS = 15) {
    String& err = s_ch[CH_MAX].err;
    HTTPClient http;
    http.setTimeout(timeoutS * 1000);
    http.setConnectTimeout(8000);
    if (!http.begin(cli, "https://platform-api.max.ru" + path)) { err = "не удалось начать запрос"; return false; }
    http.addHeader("Authorization", token(CH_MAX));
    int code;
    if (json.length()) {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(json);
    } else {
        code = http.GET();
    }
    out = code > 0 ? http.getString() : String();
    http.end();
    if (code <= 0) {
        // «connection refused» у HTTPClient — и TCP, и провал TLS: причину знает
        // только mbedTLS.
        char tls[80] = "";
        int te = cli.lastError(tls, sizeof(tls));
        err = "нет связи с platform-api.max.ru (" + HTTPClient::errorToString(code) +
              (te ? String(", TLS ") + te + ": " + tls : String("")) + ")";
        return false;
    }
    if (code == 401) { err = "токен не подходит"; return false; }
    if (code == 429) { err = "MAX просит писать реже (429)"; return false; }
    if (code != 200) { err = "ответ " + String(code); return false; }
    err = "";
    return true;
}

static void send(uint8_t ch, WiFiClientSecure& cli, const String& chat, const String& text) {
    String out;
    if (ch == CH_TG) {
        api(cli, "sendMessage", "chat_id=" + chat + "&text=" + urlenc(text) + "&disable_web_page_preview=1", out);
        return;
    }
    // MAX: до 4000 символов; режем по байтам с запасом, не посреди буквы.
    String t = text;
    if (t.length() > 3900) {
        size_t n = 3900;
        while (n && ((uint8_t)t[n] & 0xC0) == 0x80) n--;
        t = t.substring(0, n);
    }
    maxApi(cli, "/messages?chat_id=" + chat + "&disable_link_preview=true", "{\"text\":\"" + jsonEsc(t) + "\"}", out);
    vTaskDelay(pdMS_TO_TICKS(550));                // не больше двух сообщений в секунду в один чат
}

// Таблица статистики файлом. Собирает её loop() — там живёт статистика и её
// файлы, — а отправляет задача бота: s_csvReq → loop() → s_csv → задача.
// Строка на 20–30 КБ уходит в PSRAM.
static volatile bool s_csvReq = false, s_bakReq = false;
static String* volatile s_csv = nullptr;
static String s_csvCaption, s_docName, s_docType;

// Файл в чат: таблица статистики или резервная копия настроек. Собирает
// loop(), отправляет задача бота; по одному за раз.
static void csvBuild() {
    if (s_csv || (!s_csvReq && !s_bakReq)) return;
    // Файлы ходят только через Telegram: без него собирать некуда.
    if (chatId(CH_TG).isEmpty()) { s_csvReq = s_bakReq = false; return; }
    String* c = new String();
    if (s_csvReq) {
        s_csvReq = false;
        c->reserve(32768);
        statsCsv([](const String& part, void* ctx) { *(String*)ctx += part; }, c);
        time_t loc = time(nullptr) + 3 * 3600;
        struct tm lt;
        gmtime_r(&loc, &lt);
        char name[40];
        snprintf(name, sizeof(name), "solar-%04d-%02d-%02d.csv", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        s_docName = name;
        s_docType = "text/csv";
    } else {
        s_bakReq = false;
        *c = backupJson();
        s_docName = backupName();
        s_docType = "application/json";
        s_csvCaption = "🗄 Резервная копия настроек модуля. Восстановить: «Настройки» → «Модуль» → "
                       "«Резервная копия». В файле пароль Wi-Fi и токен бота — не пересылайте его.";
    }
    s_csv = c;
}

static void sendCsv(WiFiClientSecure& cli, const String& chat) {
    String* csv = s_csv;
    if (!csv) return;
    String name = s_docName;
    String bnd = "----solar" + String(esp_random(), HEX);
    String body;
    body.reserve(csv->length() + 600);
    body = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chat + "\r\n";
    body += "--" + bnd + "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" + s_csvCaption + "\r\n";
    body += "--" + bnd + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" + name +
            "\"\r\nContent-Type: " + s_docType + "\r\n\r\n";
    body += *csv;
    body += "\r\n--" + bnd + "--\r\n";
    s_csv = nullptr;
    delete csv;
    HTTPClient http;
    http.setTimeout(30000);
    http.setConnectTimeout(8000);
    if (!http.begin(cli, "https://api.telegram.org/bot" + token(CH_TG) + "/sendDocument")) return;
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + bnd);
    int code = http.POST((uint8_t*)body.c_str(), body.length());
    http.end();
    if (code != 200) s_ch[CH_TG].err = "таблица не ушла: ответ " + String(code);
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

static void handle(uint8_t ch, WiFiClientSecure& cli, const String& chat, String text) {
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

    String bound = chatId(ch);
    if (bound.isEmpty()) {                               // привязки ещё нет
        arg.trim();
        const String& code = s_ch[ch].code;
        if ((cmd == "/start" && arg == code) || cmd == code) {
            setChat(ch, chat);
            send(ch, cli, chat, String("Привязано. Буду писать сюда.\n\n") + HELP);
        } else {
            send(ch, cli, chat, "Чтобы привязать бота, отправьте /start и код со страницы модуля.");
        }
        return;
    }
    if (chat != bound) return;                           // чужие чаты — молча мимо

    if (cmd == "/status" || cmd == "/start") send(ch, cli, chat, statusText());
    else if (cmd == "/today")   send(ch, cli, chat, todayText());
    else if (cmd == "/outages") send(ch, cli, chat, outagesText());
    else if (cmd == "/boots")   send(ch, cli, chat, "🔁 " + bootText());
    else if (cmd == "/settings") send(ch, cli, chat, invSettingsText() + "\nМенять — на странице модуля: «Настройки» → «Инвертор».");
    else if (cmd == "/schedule") send(ch, cli, chat, schedText());
    else if (cmd == "/week")    { s_reportCh = ch; s_reportReq = 1; }
    else if (cmd == "/month")   { s_reportCh = ch; s_reportReq = 2; }
    else if (cmd == "/battery") { s_reportCh = ch; s_reportReq = 3; }
    else if (cmd == "/cells")   send(ch, cli, chat, paceText());
    else if ((cmd == "/backup" || cmd == "/csv") && ch != CH_TG)
        send(ch, cli, chat, "Файлы бот присылает только в Telegram. Таблица статистики и резервная копия есть "
                            "на странице модуля: «Статистика» → «Скачать CSV», «Настройки» → «Модуль» → «Резервная копия».");
    else if (cmd == "/backup") { s_bakReq = true; send(ch, cli, chat, "Собираю резервную копию, сейчас пришлю."); }
    else if (cmd == "/opp")     send(ch, cli, chat, setAsk(0, arg));
    else if (cmd == "/chp")     send(ch, cli, chat, setAsk(1, arg));
    else if (cmd == "/yes")     send(ch, cli, chat, setConfirm());
    else if (cmd == "/forecast" || cmd == "/weather") send(ch, cli, chat, weatherText());
    else if (cmd == "/csv") { s_csvCaption = "📎 Статистика по дням — открывается в Excel"; s_csvReq = true; send(ch, cli, chat, "Собираю таблицу, сейчас пришлю."); }
    else if (cmd == "/opptest") { s_oppReq = true; send(ch, cli, chat, "Тест OPP принят — начну в течение пары секунд."); }
    else                        send(ch, cli, chat, HELP);
}

static void poll(WiFiClientSecure& cli) {
    String out;
    int64_t& s_offset = s_ch[CH_TG].offset;
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
            handle(CH_TG, cli, chat, text);
        }
        pos += 12;
    }
}

// ------------------------------------------------------------------ MAX

// Разбор JSON для ответов MAX. У Telegram хватало поиска по строке, а у MAX
// ответы вложенные и порядок ключей не обещан: "chat_id" есть и у получателя,
// и в других объектах. Поэтому ключ ищется только на верхнем уровне своего
// объекта, вложенные значения перескакиваются целиком.

// Конец значения, начинающегося в p (объект, массив, строка, число, литерал).
static int jEnd(const String& s, int p) {
    int n = s.length();
    if (p < 0 || p >= n) return -1;
    char c = s[p];
    if (c == '{' || c == '[') {
        int depth = 0;
        bool str = false;
        for (int i = p; i < n; i++) {
            char k = s[i];
            if (str) { if (k == '\\') i++; else if (k == '"') str = false; continue; }
            if (k == '"') str = true;
            else if (k == '{' || k == '[') depth++;
            else if ((k == '}' || k == ']') && --depth == 0) return i + 1;
        }
        return -1;
    }
    if (c == '"') {
        for (int i = p + 1; i < n; i++) { if (s[i] == '\\') i++; else if (s[i] == '"') return i + 1; }
        return -1;
    }
    int i = p;
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isspace((uint8_t)s[i])) i++;
    return i;
}

static int jWs(const String& s, int i) { while (i < (int)s.length() && isspace((uint8_t)s[i])) i++; return i; }

// Начало значения ключа key в объекте, что открывается в obj; −1 — нет.
static int jKey(const String& s, int obj, const char* key) {
    int end = jEnd(s, obj);
    if (end < 0 || s[obj] != '{') return -1;
    int i = obj + 1;
    while (i < end) {
        i = jWs(s, i);
        if (s[i] == ',') { i++; continue; }
        if (s[i] != '"') return -1;
        int ke = jEnd(s, i);
        if (ke < 0) return -1;
        bool match = s.substring(i + 1, ke - 1) == key;
        i = jWs(s, ke);
        if (s[i] != ':') return -1;
        i = jWs(s, i + 1);
        if (match) return i;
        i = jEnd(s, i);
        if (i < 0) return -1;
    }
    return -1;
}

// Строка JSON в UTF-8; \uXXXX — в UTF-8 (суррогатные пары — смайлы — пропускаем).
static String jStr(const String& s, int p) {
    String o;
    if (p < 0 || s[p] != '"') return o;
    for (int i = p + 1; i < (int)s.length(); i++) {
        char c = s[i];
        if (c == '"') break;
        if (c != '\\') { o += c; continue; }
        char e = s[++i];
        if (e == 'n') o += '\n';
        else if (e == 't') o += '\t';
        else if (e == 'u') {
            unsigned cp = strtoul(s.substring(i + 1, i + 5).c_str(), nullptr, 16);
            i += 4;
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0xD800 || cp > 0xDFFF) {
                o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
            }
        } else o += e;
    }
    return o;
}

static String jNum(const String& s, int p) {
    if (p < 0 || !(isdigit((uint8_t)s[p]) || s[p] == '-')) return "";
    return String((long long)atoll(s.c_str() + p));
}

// Одно событие: сообщение в личном диалоге с ботом или «Начать» (bot_started;
// с payload — если открыли ссылку max.ru/<бот>?start=<код>).
static void maxUpdate(WiFiClientSecure& cli, const String& s, int u) {
    String type = jStr(s, jKey(s, u, "update_type"));
    if (type == "bot_started") {
        String chat = jNum(s, jKey(s, u, "chat_id"));
        int pl = jKey(s, u, "payload");
        if (chat.length()) handle(CH_MAX, cli, chat, "/start " + jStr(s, pl));
        return;
    }
    if (type != "message_created") return;
    int msg = jKey(s, u, "message");
    if (msg < 0 || s[msg] != '{') return;
    int snd = jKey(s, msg, "sender");
    if (snd >= 0 && s[snd] == '{') {
        int ib = jKey(s, snd, "is_bot");
        if (ib >= 0 && s[ib] == 't') return;              // своё же сообщение
    }
    int rcp = jKey(s, msg, "recipient");
    if (rcp < 0 || s[rcp] != '{') return;
    int ct = jKey(s, rcp, "chat_type");
    if (ct >= 0 && jStr(s, ct) != "dialog") return;       // только личный диалог, не группы
    String chat = jNum(s, jKey(s, rcp, "chat_id"));
    int body = jKey(s, msg, "body");
    if (!chat.length() || body < 0 || s[body] != '{') return;
    int tx = jKey(s, body, "text");
    if (tx >= 0 && s[tx] == '"') handle(CH_MAX, cli, chat, jStr(s, tx));
}

// Долгий опрос с timeout=0 — как у Telegram: соединение не держим. marker —
// указатель на следующее событие; всё до него MAX считает прочитанным.
static void maxPoll(WiFiClientSecure& cli) {
    Chan& c = s_ch[CH_MAX];
    String out;
    String path = "/updates?timeout=0&limit=20&types=message_created,bot_started";
    if (c.offset) path += "&marker=" + String((long long)c.offset);
    if (!maxApi(cli, path, "", out, 20)) return;
    int top = out.indexOf('{');
    if (top < 0) return;
    int mk = jKey(out, top, "marker");
    if (mk >= 0 && isdigit((uint8_t)out[mk])) c.offset = atoll(out.c_str() + mk);
    int arr = jKey(out, top, "updates");
    if (arr < 0 || out[arr] != '[') return;
    int end = jEnd(out, arr);
    int i = arr + 1;
    while (i > 0 && i < end) {
        i = jWs(out, i);
        if (out[i] == ',') { i++; continue; }
        if (out[i] != '{') break;
        int e = jEnd(out, i);
        if (e < 0) break;
        maxUpdate(cli, out, i);
        i = e;
    }
}

// ------------------------------------------------------------------ задача

// Один проход канала: имя бота, приветствие, проверка, файлы, очередь, опрос.
// false — до API не достучались, следующий заход через 15 с.
static bool cycle(uint8_t ch, WiFiClientSecure& cli) {
    Chan& c = s_ch[ch];
    if (!c.bot.length()) {
        String out;
        if (ch == CH_TG) {
            if (!api(cli, "getMe", "", out)) return false;
            c.bot = "@" + field(out, "username", 0);
        } else {
            if (!maxApi(cli, "/me", "", out)) return false;
            int top = out.indexOf('{');
            String u = jStr(out, jKey(out, top, "username"));
            c.bot = u.length() ? "@" + u : jStr(out, jKey(out, top, "first_name"));
        }
    }
    String chat = chatId(ch);
    if (chat.length() && !c.greeted) {
        c.greeted = true;
        send(ch, cli, chat, String(bootWasBad() ? "⚠️" : "🟢") + " Модуль на связи, сборка " + fwBuild() +
                            "\nперезапуск: " + bootReason());
    }
    if (c.test) { c.test = false; if (chat.length()) send(ch, cli, chat, "Проверка: бот работает.\n\n" + statusText()); }
    if (ch == CH_TG && chat.length() && s_csv) sendCsv(cli, chat);
    char* msg;
    while (chat.length() && xQueueReceive(c.q, &msg, 0) == pdTRUE) {
        send(ch, cli, chat, msg);
        free(msg);
    }
    if (ch == CH_TG) poll(cli);
    else maxPoll(cli);
    return true;
}

static bool pending() {
    for (uint8_t ch = 0; ch < NCH; ch++)
        if (uxQueueMessagesWaiting(s_ch[ch].q) || s_ch[ch].test) return true;
    return s_csv && chatId(CH_TG).length();
}

static void task(void*) {
    static WiFiClientSecure cli[NCH];
    cli[CH_TG].setCACert(TG_ROOT_CA);
    cli[CH_MAX].setCACert(MAX_ROOT_CA);
    uint32_t failMs[NCH] = { 0, 0 };              // когда не достучались; 0 — всё хорошо
    for (;;) {
        bool any = false;
        if (WiFi.status() == WL_CONNECTED && !otaBusy()) {       // при обновлении — молчим
            for (uint8_t ch = 0; ch < NCH; ch++) {
                if (token(ch).isEmpty()) { failMs[ch] = 0; continue; }
                any = true;
                if (failMs[ch] && millis() - failMs[ch] < 15000) continue;
                tlsLock();                       // TLS один за раз — с погодой делим
                bool ok = cycle(ch, cli[ch]);
                // Соединение не держим: закрыли — память свободна до следующего
                // опроса; пришло уведомление — очередь разбудит раньше.
                cli[ch].stop();
                tlsUnlock();
                failMs[ch] = ok ? 0 : (millis() | 1);
            }
        }
        if (!any) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
        uint32_t t0 = millis();
        while (millis() - t0 < POLL_MS && !pending()) vTaskDelay(pdMS_TO_TICKS(200));
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
    // Защиты и неисправности BMS батарей (pace.cpp) — при смене.
    static String pAlarm;
    String al = paceAlarm();
    if (al != pAlarm) {
        if (al.length()) tgNotify("❗ BMS батарей: " + al);
        else if (pAlarm.length()) tgNotify("✅ Защиты BMS батарей сняты");
        pAlarm = al;
    }
    // Разброс ячеек больше 100 мВ дольше 10 минут — какая-то ячейка отстаёт.
    static Alarm spread;
    uint16_t sp = b.cellCount > 1 ? b.spreadMv() : 0;
    alarmStep(spread, b.cellCount > 1 && sp > 100, sp < 60, 600,
              "⚠️ Разброс ячеек " + String(sp) + " мВ (ячейки " + String(b.cellMinNo) + " и " + String(b.cellMaxNo) +
              ") — какая-то отстаёт. /cells — подробно.",
              "✅ Разброс ячеек снова в норме: " + String(sp) + " мВ");
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
    if (millis() - last < 5000 || !s_mx || !anyChat()) return;
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
            s_bakReq = true;                              // следом — копия настроек
        }
    }
}

// ------------------------------------------------------------------ настройка

static void newCode(uint8_t ch) {
    char b[8];
    snprintf(b, sizeof(b), "%06lu", (unsigned long)(esp_random() % 900000 + 100000));
    s_ch[ch].code = b;
}

void tgStart() {
    s_mx = xSemaphoreCreateMutex();
    for (uint8_t ch = 0; ch < NCH; ch++) {
        newCode(ch);
        s_ch[ch].q = xQueueCreate(12, sizeof(char*));
        String st = token(ch).isEmpty() ? String("не настроен")
                  : (chatId(ch).isEmpty() ? "ждёт /start " + s_ch[ch].code : String("привязан к чату"));
        Serial.printf("[tg] %s: %s\n", ch == CH_MAX ? "MAX" : "Telegram", st.c_str());
    }
    xTaskCreatePinnedToCore(task, "tg", 12288, nullptr, 2, nullptr, 1);
}

String tgSetToken(const String& token, uint8_t ch) {
    if (ch >= NCH) return "нет такого канала";
    String t = token;
    t.trim();
    if (ch == CH_TG && t.length() && (t.indexOf(':') < 0 || t.length() < 30)) return "это не похоже на токен от @BotFather";
    if (ch == CH_MAX && t.length() && (t.length() < 20 || t.indexOf(' ') >= 0)) return "это не похоже на токен бота MAX";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    tokRef(ch) = t;
    chatRef(ch) = "";
    cfgSave();
    xSemaphoreGive(s_mx);
    Chan& c = s_ch[ch];
    c.bot = "";
    c.err = "";
    c.offset = 0;
    c.greeted = false;
    newCode(ch);
    return "";
}

void tgUnbind(uint8_t ch) {
    if (ch >= NCH) return;
    setChat(ch, "");
    newCode(ch);
}

void tgTest(uint8_t ch) { if (ch < NCH) s_ch[ch].test = true; }

static String chanJson(uint8_t ch) {
    const Chan& c = s_ch[ch];
    bool chat = chatId(ch).length();
    String j = "\"on\":" + String(token(ch).length() ? "true" : "false");
    j += ",\"bot\":\"" + jsonEsc(c.bot) + "\"";
    j += ",\"chat\":" + String(chat ? "true" : "false");
    if (!chat) j += ",\"code\":\"" + c.code + "\"";
    j += ",\"err\":\"" + jsonEsc(c.err) + "\"";
    return j;
}

// Telegram — на верхнем уровне, как было; MAX — в "max".
String tgStateJson() {
    String j = "{" + chanJson(CH_TG);
    j += ",\"low\":" + String(cfg().tgSocLow);
    j += ",\"net\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    j += ",\"max\":{" + chanJson(CH_MAX) + "}}";
    return j;
}

#endif
