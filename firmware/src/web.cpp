#include "bootlog.h"
#include "weather.h"
#include "inv_codes.h"
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
#include "stats.h"
#include "json.h"
#include "ota.h"
#include "tg.h"
#include "profile.h"
#include <Update.h>
#include <WebServer.h>
#if defined(USB_PROBE)
#include "usb_probe.h"
#endif

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
    j += ",\"mac\":\""  + jsonEsc(String(s.mac)) + "\"";
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
    j += ",\"socKnown\":" + String(s.socKnown ? "true" : "false");
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
    j += ",\"capAh\":" + String(s.capAh, 0);
    float iAvg = statsBattAvgA();
    j += ",\"iAvg\":" + String(iAvg, 1);
    j += ",\"eta\":" + String(s.socKnown ? battEtaS(s.soc, s.capAh, iAvg, cfg().tgSocLow) : 0);
    j += ",\"low\":" + String(cfg().tgSocLow);

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
    j += ",\"faultText\":\"" + jsonEsc(invCodeText(v.faultCode)) + "\"";
    j += ",\"prioOut\":" + String(v.prioOut);
    j += ",\"prioChg\":" + String(v.prioChg);
    j += ",\"battType\":" + String(v.battType);
    j += ",\"set\":" + invSetStatusJson();
    // За сегодня: счётчики самого инвертора, а нет их — статистика платы.
    float sPv, sGrid, sLoad;
    statsToday(sPv, sGrid, sLoad);
    j += ",\"pvToday\":"   + String(v.pvKwhToday   >= 0 ? v.pvKwhToday   : sPv   / 1000.0f, 2);
    j += ",\"gridToday\":" + String(v.gridKwhToday >= 0 ? v.gridKwhToday : sGrid / 1000.0f, 2);
    j += ",\"loadToday\":" + String(v.loadKwhToday >= 0 ? v.loadKwhToday : sLoad / 1000.0f, 2);
    j += ",\"todaySrc\":\"" + String(v.pvKwhToday >= 0 ? "inv" : "stats") + "\"";
    j += "},";

    j += "\"wifi\":{\"ip\":\"" + jsonEsc(netIp()) + "\",\"status\":\"" + jsonEsc(netStatus()) +
         "\",\"ap\":" + String(netIsAp() ? "true" : "false") + "},";
    j += "\"sim\":" + String(simEnabled() ? "true" : "false");
    j += ",\"fw\":\"" + String(fwBuild()) + "\",\"otaPass\":" + String(otaPasswordSet() ? "true" : "false");
    j += ",\"tg\":" + tgStateJson();
    j += ",\"boot\":" + bootJson();
    j += ",\"wx\":" + weatherJson();
    // Память: если что-то течёт (TLS бота, строки), это видно здесь со временем.
    j += ",\"heap\":{\"free\":" + String(ESP.getFreeHeap()) + ",\"max\":" + String(ESP.getMaxAllocHeap()) +
         ",\"min\":" + String(ESP.getMinFreeHeap()) + ",\"psram\":" + String(ESP.getPsramSize()) +
         ",\"psramFree\":" + String(ESP.getFreePsram()) + "},\"uptime\":" + String(millis() / 1000) + "}";
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
        bootMark("сохранены настройки");
        delay(300);
        ESP.restart();
    });

    // Поиск батареи: запуск и выдача результата. Разнесены, потому что
    // сканирование занимает пять секунд — держать на нём HTTP-запрос нельзя.
    server.on("/api/events", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        uint16_t n = server.hasArg("n") ? server.arg("n").toInt() : 20;
        if (n < 1) n = 1;
        if (n > 300) n = 300;
        server.send(200, "application/json; charset=utf-8", evJson(n));
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

#if defined(USB_PROBE)
    // Отчёт разведки USB. На прочих сборках ручки нет вовсе, и страница
    // по ответу 404 понимает, что карточку показывать не нужно.
    server.on("/api/usb", HTTP_GET, []() {
        String r = usbProbeReport(), e;
        e.reserve(r.length() + 64);
        for (size_t i = 0; i < r.length(); i++) {
            char c = r[i];
            if (c == '"' || c == '\\') { e += '\\'; e += c; }
            else if (c == '\n')        e += "\\n";
            else if ((uint8_t)c >= 0x20) e += c;
        }
        String j = "{\"state\":\"" + String(usbProbeState()) + "\",\"count\":" +
                   String(usbProbeCount()) + ",\"report\":\"" + e + "\"}";
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", j);
    });
#endif

    // Обновление прошивки со страницы — кусками по 8 КБ.
    //
    // Одним файлом не выходило: Update стирает флеш блоками по 64 КБ, на это
    // время у ESP32 замирает и приём Wi-Fi, компьютер теряет пакеты, TCP
    // растягивает паузы, а WebServer ждёт данных не дольше 5 с
    // (HTTP_MAX_POST_WAIT, не переопределяется) и бросает соединение —
    // 21.10.2026 загрузка рвалась на 64 и 786 КБ. Теперь кусок сначала целиком
    // ложится в память, во флеш пишется уже после приёма, а ответ уходит после
    // записи; браузер ждёт ответа сколько нужно. Повтор куска безопасен.
    // Буфер постоянный: 21.10.2026 malloc(16 КБ) на живой плате не нашёл
    // куска такого размера — кучу дробят Wi-Fi, BLE, USB-хост и TLS бота.
    static const size_t PART_MAX = 8192;      // постоянный, поэтому скромный: память впритык
    static uint8_t  s_part[PART_MAX];
    static size_t   s_partLen = 0, s_otaNext = 0;
    static bool     s_partOk = false, s_otaOn = false;
    static uint16_t s_rawCalls = 0;           // диагностика: сколько раз пришли данные куска
    // У запросов с сырым телом WebServer не разбирает параметры адреса —
    // arg() отдаёт оставшиеся от прошлого запроса (21.10.2026: смещение куска
    // всегда читалось как 0, пароль пропадал). Поэтому у кусков смещение и
    // пароль — в заголовках X-Off и X-Pass; пароль закодирован, как в адресе.
    static const char* HDRS[] = { "X-Off", "X-Pass" };
    server.collectHeaders(HDRS, 2);
    auto otaPass = []() -> String {
        if (!server.hasHeader("X-Pass")) return server.arg("pass");
        String in = server.header("X-Pass"), out;
        for (size_t i = 0; i < in.length(); i++) {
            if (in[i] == '%' && i + 2 < in.length()) { out += (char)strtol(in.substring(i + 1, i + 3).c_str(), nullptr, 16); i += 2; }
            else out += in[i];
        }
        return out;
    };
    auto otaAuth = [otaPass]() -> bool {
        if (otaPasswordSet() && otaPass() == cfg().webPass) return true;
        server.send(200, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"неверный пароль\"}");
        return false;
    };
    auto otaFail = [](const String& e) {
        if (Update.isRunning()) Update.abort();
        s_otaOn = false;
        server.send(200, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"" + jsonEsc(e) + "\"}");
    };

    server.on("/api/ota/begin", HTTP_GET, [otaAuth, otaFail]() {
        if (!otaAuth()) return;
        size_t size = strtoul(server.arg("size").c_str(), nullptr, 10);
        if (Update.isRunning()) Update.abort();
        if (!Update.begin(size ? size : UPDATE_SIZE_UNKNOWN)) { otaFail(Update.errorString()); return; }
        s_otaNext = 0;
        s_otaOn = true;
        Serial.printf("[ota] приём прошивки со страницы, %u байт\n", (unsigned)size);
        server.send(200, "application/json", "{\"ok\":true,\"part\":" + String(PART_MAX) + "}");
    });

    server.on("/api/ota/part", HTTP_POST, [otaAuth, otaFail]() {
        if (!otaAuth()) return;
        if (!s_otaOn) { otaFail("загрузка не начата"); return; }
        size_t off = strtoul(server.header("X-Off").c_str(), nullptr, 10);
        if (!s_partOk) {                                    // кусок не дошёл — пусть пришлют ещё раз
            server.send(200, "application/json", "{\"ok\":false,\"retry\":true,\"next\":" + String(s_otaNext) + "}");
            return;
        }
        if (s_partLen && off + s_partLen == s_otaNext) {    // этот уже записан: ответ потерялся
            server.send(200, "application/json", "{\"ok\":true,\"next\":" + String(s_otaNext) + "}");
            return;
        }
        if (!s_partLen) {
            server.send(200, "application/json", "{\"ok\":false,\"retry\":true,\"why\":\"пустой кусок\",\"calls\":" +
                        String(s_rawCalls) + ",\"clen\":\"" + server.header("Content-Length") + "\"}");
            return;
        }
        if (off != s_otaNext) { otaFail("куски пришли не по порядку"); return; }
        if (Update.write(s_part, s_partLen) != s_partLen) { otaFail(Update.errorString()); return; }
        s_otaNext += s_partLen;
        server.send(200, "application/json", "{\"ok\":true,\"next\":" + String(s_otaNext) + "}");
    }, []() {
        HTTPRaw& r = server.raw();
        if (r.status == RAW_START) {
            s_partLen = 0;
            s_partOk = false;
            s_rawCalls = 0;
        } else if (r.status == RAW_WRITE) {
            s_rawCalls++;
            if (s_partLen + r.currentSize <= PART_MAX) memcpy(s_part + s_partLen, r.buf, r.currentSize);
            s_partLen += r.currentSize;
        } else if (r.status == RAW_END) {
            s_partOk = s_partLen <= PART_MAX;
        } else {
            s_partOk = false;
        }
    });

    server.on("/api/ota/end", HTTP_GET, [otaAuth, otaFail]() {
        if (!otaAuth()) return;
        if (!s_otaOn) { otaFail("загрузка не начата"); return; }
        if (!Update.end(true)) { otaFail(Update.errorString()); return; }   // образ проверен: чип, сумма
        s_otaOn = false;
        server.sendHeader("Connection", "close");
        server.send(200, "application/json", "{\"ok\":true}");
        Serial.println("[ota] прошивка со страницы принята, перезагрузка");
        bootMark("обновление прошивки");
        delay(500);
        ESP.restart();
    });

    // Пароль на обновление. Задать с нуля может любой в домашней сети — так он
    // появляется впервые; сменить — только зная прежний.
    server.on("/api/pass", HTTP_GET, []() {
        String nw = server.arg("new"), old = server.arg("old");
        String err;
        if (nw.length() < 6)                                   err = "пароль — от 6 символов";
        else if (otaPasswordSet() && old != cfg().webPass)     err = "прежний пароль не тот";
        if (err.length()) {
            server.send(200, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"" + err + "\"}");
            return;
        }
        cfg().webPass = nw;
        cfgSave();
        otaStart();                                            // новый пароль — сразу в ArduinoOTA
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // Telegram: токен, порог, проверка, отвязка. Если пароль на обновление
    // задан, он нужен и здесь — токен даёт доступ к данным дома.
    server.on("/api/tg", HTTP_GET, []() {
        if (otaPasswordSet() && server.arg("pass") != cfg().webPass) {
            server.send(200, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"неверный пароль\"}");
            return;
        }
        String err;
        if (server.hasArg("token")) err = tgSetToken(server.arg("token"));
        if (server.hasArg("low")) {
            long low = server.arg("low").toInt();
            if (low >= 5 && low <= 90) { cfg().tgSocLow = low; cfgSave(); } else err = "порог — от 5 до 90 %";
        }
        if (server.hasArg("test"))   tgTest();
        if (server.hasArg("unbind")) tgUnbind();
        server.send(200, "application/json; charset=utf-8",
                    err.length() ? "{\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"}" : String("{\"ok\":true}"));
    });

    // Погода, панели, цена, ночная зарядка. Под паролем модуля: автоматика
    // сама переключает инвертор.
    server.on("/api/auto", HTTP_GET, []() {
        String err;
        if (!otaPasswordSet())                        err = "сначала задайте пароль модуля";
        else if (server.arg("pass") != cfg().webPass) err = "неверный пароль";
        else {
            Config& c = cfg();
            auto num = [](const char* k, float lo, float hi, float& out, String& e, const char* what) {
                if (!server.hasArg(k)) return;
                String v = server.arg(k);
                v.replace(',', '.');
                v.trim();
                float f = v.length() ? v.toFloat() : 0;
                if (f < lo || f > hi) e = String(what) + ": от " + String(lo, 0) + " до " + String(hi, 0);
                else out = f;
            };
            float lat = c.lat, lon = c.lon, kwp = c.pvKwp, price = c.price, below = c.autoBelow, from = c.autoFrom,
                  to = c.autoTo, soc = c.autoSoc;
            num("lat", -90, 90, lat, err, "широта");
            num("lon", -180, 180, lon, err, "долгота");
            num("kwp", 0, 100, kwp, err, "мощность панелей, кВт");
            num("price", 0, 100, price, err, "цена кВт·ч");
            num("below", 0, 200, below, err, "порог прогноза, кВт·ч");
            num("from", 0, 23, from, err, "начало ночи");
            num("to", 0, 23, to, err, "конец ночи");
            num("soc", 30, 100, soc, err, "заряд до");
            if (err.isEmpty()) {
                bool moved = lat != c.lat || lon != c.lon;
                c.lat = lat; c.lon = lon; c.pvKwp = kwp; c.price = price; c.autoBelow = below;
                c.autoFrom = (uint8_t)from; c.autoTo = (uint8_t)to; c.autoSoc = (uint8_t)soc;
                if (server.hasArg("on")) c.autoOn = server.arg("on") == "1";
                cfgSave();
                if (moved) weatherRefresh();
            }
        }
        server.send(200, "application/json; charset=utf-8",
                    err.length() ? "{\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"}" : String("{\"ok\":true}"));
    });

    // Вся история по дням — таблицей для Excel.
    server.on("/api/stats.csv", HTTP_GET, []() {
        char name[48];
        time_t loc = time(nullptr) + 3 * 3600;
        struct tm lt;
        gmtime_r(&loc, &lt);
        snprintf(name, sizeof(name), "attachment; filename=\"solar-%04d-%02d-%02d.csv\"", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        server.sendHeader("Content-Disposition", name);
        server.sendHeader("Cache-Control", "no-store");
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "text/csv; charset=utf-8", "");
        statsCsv([](const String& part, void*) { server.sendContent(part); }, nullptr);
        server.sendContent("");
    });

    // Статистика: day | week | month | year. Месяцы собираются из дней на лету.
    server.on("/api/stats", HTTP_GET, []() {
        String r = server.hasArg("range") ? server.arg("range") : String("day");
        if (r != "day" && r != "week" && r != "month" && r != "year") r = "day";
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", statsJson(r));
    });

    // Поток по частям: ответ на ~7 КБ не собирается в памяти целиком.
    server.on("/api/power", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "application/json; charset=utf-8", "");
        powerWrite([](const String& part) { server.sendContent(part); });
        server.sendContent("");
    });

    // Переключение настроек инвертора: what=opp|chp, code — из сверенных.
    // Только под паролем модуля; без заданного пароля запись запрещена вовсе.
    server.on("/api/inv/set", HTTP_GET, []() {
        String err;
        if (!otaPasswordSet())                              err = "сначала задайте пароль модуля";
        else if (server.arg("pass") != cfg().webPass)       err = "неверный пароль";
        else {
            String w = server.arg("what");
            uint8_t what = w == "opp" ? 0 : w == "chp" ? 1 : 9;
            err = what > 1 ? String("что переключать?") : invSetSetting(what, (uint8_t)server.arg("code").toInt());
        }
        server.send(200, "application/json; charset=utf-8",
                    err.length() ? "{\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"}" : String("{\"ok\":true}"));
    });

    // Все настройки инвертора: значения, пределы, можно ли писать и почему нет.
    server.on("/api/inv/settings", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", invSettingsJson());
    });

    // Запись одной настройки: id из таблицы, v — значение в единицах страницы.
    // Под паролем модуля; сама проверка пределов — в профиле.
    server.on("/api/inv/write", HTTP_GET, []() {
        String err;
        if (!otaPasswordSet())                        err = "сначала задайте пароль модуля";
        else if (server.arg("pass") != cfg().webPass) err = "неверный пароль";
        else err = invSettingWrite(server.arg("id"), server.arg("v"));
        server.send(200, "application/json; charset=utf-8",
                    err.length() ? "{\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"}" : String("{\"ok\":true}"));
    });

    // «Сверил с экраном — совпадает»: без этой отметки настройку не записать.
    server.on("/api/inv/verify", HTTP_GET, []() {
        String err;
        if (!otaPasswordSet())                        err = "сначала задайте пароль модуля";
        else if (server.arg("pass") != cfg().webPass) err = "неверный пароль";
        else err = invSettingVerify(server.arg("id"), server.arg("on") != "0");
        server.send(200, "application/json; charset=utf-8",
                    err.length() ? "{\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"}" : String("{\"ok\":true}"));
    });

    // Журнал обмена с инвертором и что профиль о нём узнал — текстом, для разбора.
    server.on("/api/inv", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain; charset=utf-8", invProfileReport() + "\n--- обмен ---\n" + invTrace());
    });

    server.on("/api/outages", HTTP_GET, []() {
        uint16_t n = server.hasArg("n") ? server.arg("n").toInt() : 30;
        if (n < 1) n = 1;
        if (n > 200) n = 200;
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", outagesJson(n));
    });

    // Время с телефона — когда у модуля нет интернета и NTP молчит. Своё,
    // если оно уже есть, этим не перебивается.
    server.on("/api/time", HTTP_GET, []() {
        if (server.hasArg("t")) statsSetTime((uint32_t)strtoul(server.arg("t").c_str(), nullptr, 10));
        server.send(200, "application/json", String("{\"valid\":") + (statsTimeValid() ? "true" : "false") + "}");
    });

    server.onNotFound([]() { server.send(404, "text/plain", "нет такой страницы"); });

    server.begin();
    Serial.println("[web] сервер запущен на порту 80");
}

void webLoop() {
    server.handleClient();
    // Переключение настройки завершилось — в журнал и в Telegram, с итогом.
    static uint32_t setSeq = 0;
    if (invSetSeq() != setSeq) {
        setSeq = invSetSeq();
        String m = invSetLastMsg();
        evAdd(EV_INV_SET, m.c_str());
        tgNotify("⚙️ " + m);
    }
    if (s_wifiScanReq) {
        s_wifiScanReq = false;
        netScanStart();
    }
}
