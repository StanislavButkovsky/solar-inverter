#include "net.h"
#include "cfg.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "events.h"

// Основной режим — клиент домашней сети (STA). Точка доступа поднимается только
// когда клиентом подключиться не удалось: чтобы модуль не превращался в кирпич,
// если роутер выключен, переехал или сменил пароль.
//
// Состояния:
//   STA        — сеть задана и подключена. Обычная работа.
//   AP         — сеть не задана вовсе. Ждём настройки.
//   AP + STA   — сеть задана, но подключиться не вышло. Точка доступа открыта
//                для настройки, и параллельно раз в 30 с пробуем подключиться:
//                роутер мог просто перезагружаться.
static const char* AP_SSID   = "solar-setup";
static const char* AP_PASS   = "12345678";   // WPA2 требует минимум 8 символов
static const char* MDNS_NAME = "solar";

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t RETRY_MS           = 30000;
static uint32_t       s_apGraceMs       = 120000;  // сколько держать AP после успеха
static const uint32_t AP_GRACE_AFTER_SAVE = 600000; // после смены сети — дольше
static const uint32_t AP_HOLD_MAX_MS     = 600000;  // предел паузы попыток STA

static bool     s_apUp        = false;
static bool     s_mdnsUp      = false;
static uint32_t s_lastRetry   = 0;
static uint32_t s_staUpSince  = 0;
static uint32_t s_clientSince = 0;   // когда к точке доступа подключились
static bool     s_apForced    = false;  // поднята кнопкой — сама не гаснет

// Время нужно журналу: без него события читаются как «столько-то секунд назад»,
// а это бесполезно уже через сутки. Часовой пояс — MSK, менять здесь.
static void startNtp() {
    static bool done = false;
    if (done) return;
    done = true;
    configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

static void startMdns() {
    if (s_mdnsUp) return;
    if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
        s_mdnsUp = true;
        Serial.printf("[net] доступно как http://%s.local\n", MDNS_NAME);
    }
}

static void raiseAp(bool alsoSta) {
    WiFi.mode(alsoSta ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    s_apUp = true;
    evAdd(EV_AP_UP);
    Serial.printf("[net] точка доступа \"%s\", пароль \"%s\", адрес %s\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    if (alsoSta)
        Serial.println("[net] продолжаю попытки подключиться к сохранённой сети");
}

static void dropAp() {
    if (!s_apUp || s_apForced) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_apUp = false;
    evAdd(EV_AP_DOWN);
    Serial.println("[net] точка доступа выключена, работаю клиентом");
}

void netStart() {
    WiFi.persistent(false);
    WiFi.setSleep(false);          // сон Wi-Fi мешает совместной работе с BLE

    if (cfg().ssid.isEmpty()) {
        Serial.println("[net] сеть не задана");
        raiseAp(false);
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg().ssid.c_str(), cfg().pass.c_str());
    Serial.printf("[net] подключаюсь к \"%s\"", cfg().ssid.c_str());

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        s_staUpSince = millis();
        Serial.printf("[net] подключено, адрес %s\n", WiFi.localIP().toString().c_str());
        evAdd(EV_WIFI_UP, WiFi.localIP().toString().c_str());
        startNtp();
        startMdns();

        // Сеть только что сменили из веба или приложения. Адрес выдал новый DHCP,
        // старая вкладка мертва, и узнать его человеку неоткуда. Поэтому на десять
        // минут поднимаем свою точку доступа: подключился к ней — и на той же
        // странице написан новый адрес.
        if (cfg().apAfterSave) {
            cfg().apAfterSave = false;
            cfgSave();
            s_apGraceMs = AP_GRACE_AFTER_SAVE;
            raiseAp(true);
            Serial.println("[net] точка доступа поднята на 10 минут — чтобы узнать новый адрес");
        }
        return;
    }

    Serial.println("[net] подключиться не удалось");
    raiseAp(true);                 // AP для настройки + STA продолжает пытаться
    s_lastRetry = millis();
    WiFi.begin(cfg().ssid.c_str(), cfg().pass.c_str());
}

void netLoop() {
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected) {
        if (!s_staUpSince) {
            s_staUpSince = millis();
            Serial.printf("[net] подключено, адрес %s\n", WiFi.localIP().toString().c_str());
            evAdd(EV_WIFI_UP, WiFi.localIP().toString().c_str());
            startNtp();
            startMdns();
        }
        // Точку доступа гасим не сразу: у того, кто сейчас настраивает модуль
        // с телефона, должно остаться время увидеть выданный адрес.
        if (s_apUp && millis() - s_staUpSince > s_apGraceMs) dropAp();
        return;
    }

    s_staUpSince = 0;
    if (cfg().ssid.isEmpty()) return;          // настраивать нечего, ждём в AP

    // Пока кто-то сидит на нашей точке доступа, попытки подключиться к
    // сохранённой сети приостановлены. Причина физическая: у ESP32 одна антенна,
    // точка доступа и клиент обязаны быть на одном канале, и каждая попытка
    // подключения уводит канал за собой — телефон настройщика в этот момент
    // отваливается. Ровно тогда, когда он настраивает.
    // Предел на всякий случай: телефон могли забыть подключённым.
    if (WiFi.softAPgetStationNum() > 0) {
        if (!s_clientSince) s_clientSince = millis();
        if (millis() - s_clientSince < AP_HOLD_MAX_MS) {
            s_lastRetry = millis();            // отсчёт паузы начинается заново
            return;
        }
    } else {
        s_clientSince = 0;
    }

    if (millis() - s_lastRetry > RETRY_MS) {
        s_lastRetry = millis();
        if (!s_apUp) {
            // Работали клиентом и потеряли сеть. Поднимаем точку доступа, иначе
            // при смене пароля на роутере до модуля будет не достучаться.
            Serial.println("[net] сеть потеряна");
            evAdd(EV_WIFI_DOWN, cfg().ssid.c_str());
            raiseAp(true);
        }
        WiFi.disconnect();
        WiFi.begin(cfg().ssid.c_str(), cfg().pass.c_str());
    }
}

static bool     s_scanPending = false;
static uint32_t s_scanStarted = 0;
static String   s_lastScan;

// Кнопкой точка доступа и поднимается, и убирается. Поднятая вручную сама
// не гаснет: её включают именно тогда, когда до модуля иначе не достучаться.
bool netToggleAp() {
    if (s_apForced || s_apUp) {
        s_apForced = false;
        if (WiFi.status() == WL_CONNECTED) {
            dropAp();
        } else {
            // Без сети выключать точку доступа некуда — оставляем как есть,
            // иначе модуль стал бы недоступен полностью.
            Serial.println("[net] сети нет, точку доступа оставляю");
            s_apForced = true;
            return true;
        }
        return false;
    }
    s_apForced = true;
    raiseAp(!cfg().ssid.isEmpty());
    Serial.println("[net] точка доступа поднята кнопкой, держится до выключения или перезагрузки");
    return true;
}

bool netApActive() { return s_apUp; }

void netScanStart() {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);     // асинхронно, скрытые сети не нужны
    s_scanPending = true;
    s_scanStarted = millis();
}

String netScanJson() {
    int n = WiFi.scanComplete();

    // Сразу после запуска scanComplete() успевает вернуть прошлое значение,
    // и страница мигнула бы старым списком или пустотой вместо «ищу».
    // Первую секунду после запроса отвечаем «занят» независимо от него.
    if (s_scanPending && millis() - s_scanStarted < 1200)
        return "{\"busy\":true,\"nets\":[]}";

    if (n == WIFI_SCAN_RUNNING) return "{\"busy\":true,\"nets\":[]}";
    s_scanPending = false;

    // Результаты забираются один раз и удаляются, поэтому держим копию: иначе
    // второй открытый браузер (или повторный опрос) получит пустой список.
    if (n < 0) return s_lastScan.length() ? s_lastScan : String("{\"busy\":false,\"nets\":[]}");

    static const int MAX = 24;
    int   idx[MAX];
    int   cnt = 0;

    // Одна и та же сеть часто видна с нескольких точек — оставляем сильнейшую,
    // иначе список превращается в кашу из одинаковых имён.
    for (int i = 0; i < n && cnt < MAX; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;
        int dup = -1;
        for (int k = 0; k < cnt; k++)
            if (WiFi.SSID(idx[k]) == WiFi.SSID(i)) { dup = k; break; }
        if (dup >= 0) {
            if (WiFi.RSSI(i) > WiFi.RSSI(idx[dup])) idx[dup] = i;
        } else {
            idx[cnt++] = i;
        }
    }

    for (int a = 0; a < cnt - 1; a++)          // сильные наверх
        for (int b = a + 1; b < cnt; b++)
            if (WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[a])) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }

    String j = "{\"busy\":false,\"nets\":[";
    for (int k = 0; k < cnt; k++) {
        int i = idx[k];
        String name = WiFi.SSID(i);
        name.replace("\\", "");
        name.replace("\"", "'");
        if (k) j += ",";
        j += "{\"ssid\":\"" + name + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
             ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    j += "]}";

    WiFi.scanDelete();
    s_lastScan = j;
    return j;
}

bool netIsAp() { return s_apUp && WiFi.status() != WL_CONNECTED; }

String netIp() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    return s_apUp ? WiFi.softAPIP().toString() : String("0.0.0.0");
}

String netStatus() {
    if (WiFi.status() == WL_CONNECTED) {
        String s = "клиент сети " + WiFi.SSID() + ", RSSI " + String(WiFi.RSSI()) + " дБм";
        if (s_apUp) s += s_apForced ? " (точка доступа поднята кнопкой)"
                                    : " (точка доступа ещё открыта)";
        return s;
    }
    if (s_apUp) {
        String s = "точка доступа " + String(AP_SSID);
        int n = WiFi.softAPgetStationNum();
        if (n > 0) s += ", подключено устройств: " + String(n);
        if (cfg().ssid.isEmpty()) s += ", сеть не задана";
        else if (n > 0)           s += ", попытки подключиться к \"" + cfg().ssid + "\" на паузе";
        else                      s += ", подключаюсь к \"" + cfg().ssid + "\"";
        return s;
    }
    return "нет связи";
}
