// Модуль мониторинга батареи: DALY BMS по BLE → веб-интерфейс в локальной сети.
//
// Ядро 0: BLE-центральный (опрос батареи)
// Ядро 1: Wi-Fi, веб-сервер, BLE-периферийный (настройка сети), CLI, демо-данные
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include "state.h"
#include "cfg.h"
#include "net.h"
#include "web.h"
#include "daly.h"
#include "prov.h"
#include "cli.h"
#include "sim.h"
#include "button.h"
#include "events.h"

static String deviceName() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "Solar-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    stateInit();
    cfgInit();
    buttonInit();

    String name = deviceName();

    // NimBLE поднимаем один раз здесь: и центральный, и периферийный работают
    // поверх общего стека.
    NimBLEDevice::init(name.c_str());
    NimBLEDevice::setPower(9);   // дБм

    netStart();
    webStart();
    provStart(name);
    simStart();
    dalyStart();

    evAdd(EV_BOOT);
    cliBanner();
    Serial.print("> ");
}

void loop() {
    webLoop();
    netLoop();
    provLoop();
    cliLoop();
    buttonLoop();
    evWatchLoop();
    delay(2);
}
