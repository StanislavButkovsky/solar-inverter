#include "tg.h"
#include "tg_roots.h"
#include "cfg.h"
#include "state.h"
#include "stats.h"
#include "ota.h"
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
    r += battLine();
    return r;
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
    "Команды:\n/status — что сейчас\n/today — итоги дня\n/outages — последние отключения сети\n/help — этот список";

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

static void handle(WiFiClientSecure& cli, const String& chat, String text) {
    text.trim();
    int sp = text.indexOf(' ');
    String cmd = sp < 0 ? text : text.substring(0, sp);
    String arg = sp < 0 ? String("") : text.substring(sp + 1);
    int at = cmd.indexOf('@');
    if (at > 0) cmd = cmd.substring(0, at);
    cmd.toLowerCase();

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
        if (!s_bot.length()) {
            String out;
            if (api(cli, "getMe", "", out)) s_bot = "@" + field(out, "username", 0);
            else { vTaskDelay(pdMS_TO_TICKS(15000)); continue; }
        }
        String chat = chatId();
        if (chat.length() && !greeted) {
            greeted = true;
            send(cli, chat, String("🟢 Модуль на связи, сборка ") + fwBuild());
        }
        if (s_testReq) { s_testReq = false; if (chat.length()) send(cli, chat, "Проверка: бот работает.\n\n" + statusText()); }
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
        uint32_t t0 = millis();
        while (millis() - t0 < POLL_MS && !uxQueueMessagesWaiting(s_q) && !s_testReq) vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ------------------------------------------------------------------ пороги

void tgLoop() {
    static uint32_t last = 0, lastFresh = 0;
    static bool lowSent = false, linkSent = false;
    static uint8_t prevFault = 0;
    static int32_t summaryDay = -1;
    if (millis() - last < 5000 || !s_mx || chatId().isEmpty()) return;
    last = millis();

    InvState v = invSnapshot();
    BmsState b = stateSnapshot();

    if (b.fresh && b.socKnown) {
        if (!lowSent && b.soc < cfg().tgSocLow) {
            lowSent = true;
            tgNotify("🔋 Батарея " + String(b.soc, 0) + " % — ниже порога " + String(cfg().tgSocLow) + " %\n" + battLine());
        } else if (lowSent && b.soc > cfg().tgSocLow + 5) {
            lowSent = false;
        }
    }
    if (v.fresh) {
        if (v.faultCode != prevFault) {
            tgNotify(v.faultCode ? "❗ Авария инвертора, код " + String(v.faultCode) : String("✅ Авария инвертора снята"));
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
        if (lt.tm_hour == 21 && day != summaryDay) { summaryDay = day; tgNotify(todayText()); }
        if (summaryDay < 0) summaryDay = lt.tm_hour >= 21 ? day : day - 1;   // не слать сводку сразу после включения
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
