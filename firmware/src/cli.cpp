#include "cli.h"
#include "cfg.h"
#include "net.h"
#include "state.h"
#include "daly.h"
#include "sim.h"
#include <WiFi.h>

static String buf;

void cliBanner() {
    Serial.println();
    Serial.println("=== модуль мониторинга батареи ===");
    Serial.println("наберите help и Enter");
}

static void help() {
    Serial.println(F(
        "help                  этот список\n"
        "status                состояние целиком\n"
        "wifi scan             список сетей\n"
        "wifi set <ssid> <pass>  задать сеть и перезагрузиться\n"
        "wifi status           состояние сети\n"
        "ble scan              поиск устройств BLE рядом\n"
        "ble mac <mac>         привязать батарею по адресу (пусто — искать самому)\n"
        "ble release <мин>     отпустить батарею официальному приложению\n"
        "sim on|off            демо-данные для показа интерфейса без батареи\n"
        "poll <мс>             пауза между запросами к BMS\n"
        "reboot                перезагрузка\n"
        "factoryreset          стереть настройки"));
}

static void printStatus() {
    BmsState s = stateSnapshot();
    Serial.println("--- сеть ---");
    Serial.printf("  %s\n  адрес: http://%s\n", netStatus().c_str(), netIp().c_str());
    Serial.println("--- батарея ---");
    if (simEnabled()) Serial.println("  ВНИМАНИЕ: показаны демо-данные (sim on)");
    Serial.printf("  связь: %s%s\n", s.linked ? "есть" : "нет",
                  s.linked && !s.fresh ? " (данные устарели)" : "");
    if (s.mac[0]) Serial.printf("  устройство: %s, RSSI %d дБм\n", s.mac, s.rssi);
    Serial.printf("  кадров: %u ок / %u с ошибкой / %u разрывов\n",
                  s.framesOk, s.framesBad, s.disconnects);
    if (s.cellCount) {
        Serial.printf("  %.2f В  %+.1f А  %.0f Вт  SOC %.1f%%\n",
                      s.voltage, s.current, s.power(), s.soc);
        Serial.printf("  ячеек %u, разброс %u мВ (мин %u мВ №%u, макс %u мВ №%u)\n",
                      s.cellCount, s.spreadMv(), s.cellMinMv, s.cellMinNo,
                      s.cellMaxMv, s.cellMaxNo);
        Serial.print("  ");
        for (uint8_t i = 0; i < s.cellCount; i++) Serial.printf("%u ", s.cellMv[i]);
        Serial.println();
        Serial.printf("  температура %d…%d °C, остаток %.2f А·ч\n",
                      s.tempMin, s.tempMax, s.remainingMah / 1000.0);
    }
    if (dalyIsReleased())
        Serial.printf("  батарея отпущена приложению ещё на %u с\n", dalyReleaseLeftS());
}

static void exec(String line) {
    line.trim();
    if (!line.length()) return;

    if (line == "help")   { help(); return; }
    if (line == "status") { printStatus(); return; }
    if (line == "reboot") { Serial.println("перезагрузка"); delay(200); ESP.restart(); }
    if (line == "factoryreset") {
        cfgFactoryReset();
        Serial.println("настройки стёрты, перезагрузка");
        delay(200); ESP.restart();
    }

    if (line == "wifi scan") {
        Serial.println("ищу сети…");
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++)
            Serial.printf("  %-32s %4d дБм %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "открытая" : "");
        WiFi.scanDelete();
        return;
    }
    if (line == "wifi status") { Serial.println(netStatus()); return; }

    if (line.startsWith("wifi set ")) {
        String rest = line.substring(9);
        rest.trim();
        int sp = rest.indexOf(' ');
        cfg().ssid = (sp < 0) ? rest : rest.substring(0, sp);
        cfg().pass = (sp < 0) ? ""   : rest.substring(sp + 1);
        cfg().apAfterSave = true;
        cfgSave();
        Serial.printf("сеть \"%s\" сохранена, перезагрузка\n", cfg().ssid.c_str());
        delay(300); ESP.restart();
    }

    if (line == "ble scan") {
        Serial.println("сканирую эфир 6 с…");
        std::vector<BleFound> v = dalyScan(6);
        for (size_t i = 0; i < v.size(); i++)
            Serial.printf("  %s %4d дБм  %-24s %s\n", v[i].mac.c_str(), v[i].rssi,
                          v[i].name.length() ? v[i].name.c_str() : "(без имени)",
                          v[i].hasDalyService ? "<< похоже на DALY" : "");
        if (v.empty()) Serial.println("  пусто");
        return;
    }

    if (line.startsWith("ble mac")) {
        String m = line.substring(7); m.trim();
        cfg().bmsMac = m; cfgSave();
        Serial.printf("адрес батареи: \"%s\" (перезагрузите для применения)\n", m.c_str());
        return;
    }

    if (line.startsWith("ble release")) {
        String m = line.substring(11); m.trim();
        uint32_t mins = m.length() ? m.toInt() : 10;
        dalyReleaseFor(mins);
        Serial.printf("батарея отпущена на %u мин\n", mins);
        return;
    }

    if (line == "sim on" || line == "sim off") {
        bool on = line.endsWith("on");
        simSetEnabled(on);
        Serial.println(on ? "демо-данные включены — интерфейс покажет пометку"
                          : "демо-данные выключены");
        return;
    }

    if (line.startsWith("poll ")) {
        cfg().pollMs = line.substring(5).toInt();
        if (cfg().pollMs < 100) cfg().pollMs = 100;
        cfgSave();
        Serial.printf("пауза опроса %u мс\n", cfg().pollMs);
        return;
    }

    Serial.println("не понял, наберите help");
}

void cliLoop() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { Serial.println(); exec(buf); buf = ""; Serial.print("> "); }
        else if (c == 8 || c == 127) { if (buf.length()) { buf.remove(buf.length() - 1); Serial.print("\b \b"); } }
        else { buf += c; Serial.print(c); }
    }
}
