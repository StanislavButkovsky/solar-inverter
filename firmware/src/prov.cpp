#include "prov.h"
#include "cfg.h"
#include "net.h"
#include <NimBLEDevice.h>
#include <WiFi.h>

static const char* SVC_UUID  = "9a1f0001-7f3a-4b0e-9f2b-1c7a5d3e0001";
static const char* SSID_UUID = "9a1f0002-7f3a-4b0e-9f2b-1c7a5d3e0001";
static const char* PASS_UUID = "9a1f0003-7f3a-4b0e-9f2b-1c7a5d3e0001";
static const char* CTRL_UUID = "9a1f0004-7f3a-4b0e-9f2b-1c7a5d3e0001";
static const char* STAT_UUID = "9a1f0005-7f3a-4b0e-9f2b-1c7a5d3e0001";

static NimBLECharacteristic* s_stat = nullptr;
static String  s_name;
static String  s_pendSsid, s_pendPass;
static volatile bool s_doScan  = false;
static volatile bool s_doApply = false;
static volatile bool s_doReset = false;

static void notifyLine(const String& line) {
    if (!s_stat) return;
    s_stat->setValue((uint8_t*)line.c_str(), line.length());
    s_stat->notify();
    delay(30);   // не захлёбываться: у телефона очередь уведомлений короткая
}

static void notifyState() {
    notifyLine("STATE|" + netStatus() + "|" + netIp());
}

class CtrlCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        NimBLEAttValue v = c->getValue();
        if (v.length() == 0) return;
        switch ((uint8_t)v[0]) {
        case 1: s_doScan  = true; break;
        case 2: s_doApply = true; break;
        case 3: s_doReset = true; break;
        default: break;
        }
    }
};

class SsidCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        s_pendSsid = String(c->getValue().c_str());
        Serial.printf("[prov] имя сети: %s\n", s_pendSsid.c_str());
    }
};

class PassCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        s_pendPass = String(c->getValue().c_str());
        Serial.println("[prov] пароль получен");
    }
};

class SrvCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* srv, NimBLEConnInfo& info) override {
        Serial.println("[prov] телефон подключился");
        // продолжаем рекламу: к нам может прийти и второй клиент
        NimBLEDevice::startAdvertising();
    }
    void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& info, int reason) override {
        Serial.println("[prov] телефон отключился");
        NimBLEDevice::startAdvertising();
    }
};

void provStart(const String& deviceName) {
    s_name = deviceName;

    NimBLEServer* srv = NimBLEDevice::createServer();
    srv->setCallbacks(new SrvCb());

    NimBLEService* svc = srv->createService(SVC_UUID);

    svc->createCharacteristic(SSID_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(new SsidCb());
    svc->createCharacteristic(PASS_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(new PassCb());
    svc->createCharacteristic(CTRL_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(new CtrlCb());

    s_stat = svc->createCharacteristic(
        STAT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    s_stat->setValue("STATE|запуск|");

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->enableScanResponse(true);
    adv->start();

    Serial.printf("[prov] реклама BLE как \"%s\"\n", s_name.c_str());
}

void provLoop() {
    if (s_doScan) {
        s_doScan = false;
        Serial.println("[prov] ищу сети Wi-Fi");
        int n = WiFi.scanNetworks(false, false);
        for (int i = 0; i < n && i < 20; i++) {
            String line = "NET|" + String(WiFi.RSSI(i)) + "|" +
                          String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 1 : 0) + "|" +
                          WiFi.SSID(i);
            notifyLine(line);
        }
        notifyLine("NET|END");
        WiFi.scanDelete();
        notifyState();
    }

    if (s_doApply) {
        s_doApply = false;
        if (s_pendSsid.length()) {
            cfg().ssid = s_pendSsid;
            cfg().pass = s_pendPass;
            cfg().apAfterSave = true;
            cfgSave();
            notifyLine("STATE|сохранено, перезагружаюсь|");
            Serial.println("[prov] настройки сохранены, перезагрузка");
            delay(400);
            ESP.restart();
        } else {
            notifyLine("STATE|имя сети не задано|");
        }
    }

    if (s_doReset) {
        s_doReset = false;
        cfgFactoryReset();
        notifyLine("STATE|сброшено, перезагружаюсь|");
        delay(400);
        ESP.restart();
    }

    // раз в пять секунд обновляем состояние для подключённого телефона
    static uint32_t last = 0;
    if (millis() - last > 5000) {
        last = millis();
        notifyState();
    }
}

String provDeviceName() { return s_name; }
