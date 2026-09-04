#include "web.h"
#include "webui_gz.h"
#include "icon.h"
#include "state.h"
#include "cfg.h"
#include "net.h"
#include "daly.h"
#include "sim.h"
#include "events.h"
#include "inv.h"
#include <WebServer.h>

// Синхронный сервер, а не асинхронный: опрос раз в секунду он тянет с запасом,
// зависимостей ноль, и он заметно спокойнее уживается с активным BLE.
static WebServer server(80);

// Отложенный запуск сканирования: см. обработчик /api/wifi/scan.
static volatile bool s_wifiScanReq = false;

static String jsonState() {
    BmsState s = stateSnapshot();
    String j;
    j.reserve(1400);

    uint32_t age = s.lastFrameMs ? (millis() - s.lastFrameMs) / 1000 : 0;

    j += "{\"link\":{";
    j += "\"linked\":"  + String(s.linked ? "true" : "false");
    j += ",\"fresh\":"  + String(s.fresh ? "true" : "false");
    j += ",\"ageS\":"   + (s.lastFrameMs ? String(age) : String("null"));
    j += ",\"rssi\":"   + String(s.rssi);
    j += ",\"mac\":\""  + String(s.mac) + "\"";
    j += ",\"ok\":"     + String(s.framesOk);
    j += ",\"bad\":"    + String(s.framesBad);
    j += ",\"disc\":"   + String(s.disconnects);
    j += ",\"released\":" + String(dalyIsReleased() ? "true" : "false");
    j += ",\"releaseLeft\":" + String(dalyReleaseLeftS());
    j += "},";

    j += "\"bms\":{";
    j += "\"v\":"       + String(s.voltage, 2);
    j += ",\"i\":"      + String(s.current, 1);
    j += ",\"p\":"      + String(s.power(), 0);
    j += ",\"soc\":"    + String(s.soc, 1);
    j += ",\"cellCount\":" + String(s.cellCount);
    j += ",\"maxMv\":"  + String(s.cellMaxMv);
    j += ",\"minMv\":"  + String(s.cellMinMv);
    j += ",\"spread\":" + String(s.spreadMv());
    j += ",\"tMax\":"   + String(s.tempMax);
    j += ",\"tMin\":"   + String(s.tempMin);
    j += ",\"state\":"  + String(s.chargeState);
    j += ",\"chgMos\":" + String(s.chargeMos ? "true" : "false");
    j += ",\"dsgMos\":" + String(s.dischargeMos ? "true" : "false");
    j += ",\"remainAh\":" + String(s.remainingMah / 1000.0, 2);

    j += ",\"cells\":[";
    for (uint8_t i = 0; i < s.cellCount && i < MAX_CELLS; i++) {
        if (i) j += ",";
        j += String(s.cellMv[i]);
    }
    j += "]";

    j += ",\"balance\":[";
    for (uint8_t i = 0; i < s.cellCount && i < MAX_CELLS; i++) {
        if (i) j += ",";
        j += String((s.balance[i / 8] >> (i % 8)) & 1);
    }
    j += "]";

    // Флаги аварий отдаём как есть: расшифровка битов сверяется с приложением
    // на этапе 4, до тех пор врать подписями хуже, чем показать сырьё.
    bool anyFault = false;
    for (uint8_t i = 0; i < 7; i++) if (s.faults[i]) anyFault = true;
    if (s.haveFaults && anyFault) {
        char hex[32];
        snprintf(hex, sizeof(hex), "%02X %02X %02X %02X %02X %02X %02X",
                 s.faults[0], s.faults[1], s.faults[2], s.faults[3],
                 s.faults[4], s.faults[5], s.faults[6]);
        j += ",\"faults\":\"" + String(hex) + "\"";
    } else {
        j += ",\"faults\":null";
    }
    j += "},";

    InvState v = invSnapshot();
    j += "\"inv\":{";
    j += "\"linked\":" + String(v.linked ? "true" : "false");
    j += ",\"fresh\":"  + String(v.fresh ? "true" : "false");
    j += ",\"mode\":"   + String(v.mode);
    j += ",\"grid\":"   + String(v.gridOn ? "true" : "false");
    j += ",\"gridV\":"  + String(v.gridV, 1);
    j += ",\"gridHz\":" + String(v.gridHz, 1);
    j += ",\"gridW\":"  + String(v.gridW, 0);
    j += ",\"outV\":"   + String(v.outV, 1);
    j += ",\"outHz\":"  + String(v.outHz, 1);
    j += ",\"outW\":"   + String(v.outW, 0);
    j += ",\"loadPct\":" + String(v.loadPct);
    j += ",\"pvV\":"    + String(v.pvV, 1);
    j += ",\"pvW\":"    + String(v.pvW, 0);
    j += ",\"chgW\":"   + String(v.chgW, 0);
    j += ",\"tempC\":"  + String(v.tempC);
    j += ",\"fault\":"  + String(v.faultCode);
    j += ",\"prioOut\":" + String(v.prioOut);
    j += ",\"prioChg\":" + String(v.prioChg);
    j += "},";

    j += "\"wifi\":{\"ip\":\"" + netIp() + "\",\"status\":\"" + netStatus() +
         "\",\"ap\":" + String(netIsAp() ? "true" : "false") + "},";
    j += "\"sim\":" + String(simEnabled() ? "true" : "false") + "}";
    return j;
}

static void sendIcon() {
    server.sendHeader("Cache-Control", "max-age=604800");
    server.send_P(200, "image/png", (const char*)ICON_PNG, ICON_PNG_LEN);
}

void webStart() {
    server.on("/", HTTP_GET, []() {
        // Страница лежит в прошивке уже сжатой: втрое меньше флеша и заметно
        // быстрее грузится на планшете. Распаковку берёт на себя браузер.
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("Content-Encoding", "gzip");
        server.send_P(200, "text/html; charset=utf-8",
                      (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    });

    // Ярлык на главном экране. iOS берёт apple-touch-icon и только PNG,
    // Android — иконку из манифеста. Отдаём один файл под всеми именами.
    server.on("/icon.png", HTTP_GET, sendIcon);
    server.on("/apple-touch-icon.png", HTTP_GET, sendIcon);
    server.on("/apple-touch-icon-precomposed.png", HTTP_GET, sendIcon);
    server.on("/favicon.ico", HTTP_GET, sendIcon);

    server.on("/manifest.json", HTTP_GET, []() {
        static const char MANIFEST[] PROGMEM =
            "{\"name\":\"Батарея\",\"short_name\":\"Батарея\",\"start_url\":\"/\","
            "\"display\":\"standalone\",\"orientation\":\"portrait\","
            "\"background_color\":\"#10131a\",\"theme_color\":\"#10131a\","
            "\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"192x192\",\"type\":\"image/png\","
            "\"purpose\":\"any maskable\"}]}";
        server.sendHeader("Cache-Control", "max-age=86400");
        server.send_P(200, "application/manifest+json; charset=utf-8", MANIFEST);
    });

    server.on("/api/state", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", jsonState());
    });

    server.on("/api/ble/release", HTTP_GET, []() {
        uint32_t m = server.hasArg("min") ? server.arg("min").toInt() : 10;
        if (m < 1)  m = 1;
        if (m > 60) m = 60;
        dalyReleaseFor(m);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/wifi/scan", HTTP_GET, []() {
        // Только помечаем: само сканирование запустится из webLoop(), когда
        // ответ уже уйдёт и соединение закроется. Запуск прямо здесь съедает
        // ответ — радио уходит скакать по каналам, не успев его отправить,
        // и страница остаётся ждать вечно.
        s_wifiScanReq = true;
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/wifi/found", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", netScanJson());
    });

    server.on("/api/config", HTTP_GET, []() {
        if (server.hasArg("ssid"))   cfg().ssid   = server.arg("ssid");
        if (server.hasArg("pass"))   cfg().pass   = server.arg("pass");
        if (server.hasArg("bmsmac")) cfg().bmsMac = server.arg("bmsmac");
        // Просим следующую загрузку поднять точку доступа: иначе новый адрес,
        // выданный DHCP другой сети, узнать будет неоткуда.
        if (server.hasArg("ssid")) cfg().apAfterSave = true;
        cfgSave();
        server.send(200, "application/json", "{\"ok\":true}");
        delay(300);
        ESP.restart();
    });

    // Поиск батареи: запуск и выдача результата. Разнесены, потому что
    // сканирование занимает пять секунд — держать на нём HTTP-запрос нельзя.
    server.on("/api/events", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", evJson());
    });

    server.on("/api/ble/scan", HTTP_GET, []() {
        dalyRequestScan();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/ble/found", HTTP_GET, []() {
        std::vector<BleFound> v = dalyScanResults();
        String j = "{\"busy\":" + String(dalyScanBusy() ? "true" : "false") +
                   ",\"bound\":\"" + cfg().bmsMac + "\"" +
                   ",\"boundInv\":\"" + cfg().invMac + "\",\"devices\":[";
        for (size_t i = 0; i < v.size(); i++) {
            if (i) j += ",";
            String name = v[i].name;
            name.replace("\"", "'");
            j += "{\"mac\":\"" + v[i].mac + "\",\"name\":\"" + name +
                 "\",\"rssi\":" + String(v[i].rssi) +
                 ",\"daly\":" + String(v[i].hasDalyService ? "true" : "false") +
                 ",\"inv\":" + String(invLooksLikeDongle(v[i].name, v[i].hasEybond) ? "true" : "false") + "}";
        }
        j += "]}";
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", j);
    });

    server.on("/api/ble/bind", HTTP_GET, []() {
        String mac = server.hasArg("mac") ? server.arg("mac") : String("");
        if (server.hasArg("what") && server.arg("what") == "inv") invBind(mac);
        else                                                      dalyBind(mac);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/sim", HTTP_GET, []() {
        bool on = server.hasArg("on") && server.arg("on") == "1";
        simSetEnabled(on);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.onNotFound([]() { server.send(404, "text/plain", "нет такой страницы"); });

    server.begin();
    Serial.println("[web] сервер запущен на порту 80");
}

void webLoop() {
    server.handleClient();
    if (s_wifiScanReq) {
        s_wifiScanReq = false;
        netScanStart();
    }
}
