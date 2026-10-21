// Модуль мониторинга батареи: DALY BMS по BLE → веб-интерфейс в локальной сети.
//
// Ядро 0: BLE-центральный (опрос батареи)
// Ядро 1: Wi-Fi, веб-сервер, терминал, демо-данные
#include <Arduino.h>
#if !defined(NO_BLE)
#include <NimBLEDevice.h>
#endif
#include "esp_bt.h"
#include <WiFi.h>

#include "state.h"
#include "cfg.h"
#include "net.h"
#include "web.h"
#include "daly.h"
#include "cli.h"
#include "sim.h"
#include "button.h"
#include "events.h"
#include "stats.h"
#include "rtc.h"
#include "ota.h"
#include "tg.h"
#if defined(USB_PROBE)
#include "usb_probe.h"
#endif

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

    // NimBLE работает только центральным: настройка сети идёт через точку
    // доступа, поэтому рекламировать себя модулю незачем.
    // У HS35 батарея штатная и говорит через инвертор: Bluetooth нужен только
    // для привязанной батареи DALY. Без неё стек NimBLE и контроллер держали
    // десятки килобайт внутренней памяти впустую, а на рукопожатии TLS бота
    // свободной оставалось 140 байт (21.10.2026). Не привязана — Bluetooth не
    // поднимаем и отдаём его память системе до перезагрузки.
#if defined(NO_BLE)
    // Сборка без Bluetooth (HS35, Asterion): стека нет, память контроллера — системе.
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    (void)name;
#else
    bool ble = true;
#if defined(TARGET_HS35)
    ble = !cfg().bmsMac.isEmpty();
#endif
    if (ble) {
        NimBLEDevice::init(name.c_str());
        NimBLEDevice::setPower(9);   // дБм
    } else {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        Serial.println("[ble] батарея по Bluetooth не привязана — Bluetooth выключен, память отдана");
    }
    dalySetBle(ble);
#endif

    netStart();
    webStart();
    otaStart();
    simStart();
    dalyStart();
    rtcStart();               // до статистики: с часами она сразу знает, какой сегодня день
    statsStart();
    tgStart();
#if defined(USB_PROBE)
    // Сборка разведки (esp32s3-probe): USB инвертора обслуживает не опрос,
    // а разведка — она опознаёт устройство и сохраняет отчёт в NVS.
    usbProbeStart();
#endif

    evAdd(EV_BOOT);
    cliBanner();
    Serial.print("> ");
}

void loop() {
    webLoop();
    netLoop();
    cliLoop();
    buttonLoop();
    evWatchLoop();
    statsLoop();
    rtcLoop();
    otaLoop();
    tgLoop();
    delay(2);
}
