// Транспорт до инвертора по Bluetooth: донгл Eybond WFBLE.DTU.
//
//   сервис        53300000-0023-4bd4-bbd5-a6920e4c5653
//   53300001-…    запись кадров
//   53300005-…    уведомления с ответами
//
// Донгл держит одно соединение: пока его занимает модуль, приложение
// SmartESS к инвертору не подключится, и наоборот.
#if !defined(TARGET_HS35)

#include "inv.h"
#include "inv_transport.h"
#include "cfg.h"
#include <NimBLEDevice.h>

static const NimBLEUUID SVC_UUID("53300000-0023-4bd4-bbd5-a6920e4c5653");
static const NimBLEUUID CHR_WRITE("53300001-0023-4bd4-bbd5-a6920e4c5653");
static const NimBLEUUID CHR_NOTIFY("53300005-0023-4bd4-bbd5-a6920e4c5653");

static NimBLEClient*               s_client = nullptr;
static NimBLERemoteCharacteristic* s_tx     = nullptr;
static volatile bool               s_connected = false;

static void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t n, bool) {
    invOnBytes(data, n);
}

class InvCb : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override { s_connected = true; }
    void onDisconnect(NimBLEClient*, int) override {
        s_connected = false;
        s_tx = nullptr;
        Serial.println("[инв] соединение потеряно");
    }
};

bool invLooksLikeDongle(const String& name, bool hasService) {
    if (hasService) return true;
    // Донглы Eybond рекламируются серийным номером: длинная строка из цифр.
    if (name.length() < 8) return false;
    for (uint16_t i = 0; i < name.length(); i++)
        if (!isdigit(name[i])) return false;
    return true;
}

void invTrBegin() { }

bool invTrConnected() { return s_connected; }

bool invTrConnect() {
    String want = cfg().invMac;
    want.toLowerCase();
    if (!want.length()) return false;               // донгл не привязан

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults res = scan->getResults(4000, false);

    bool found = false;
    NimBLEAddress addr;
    for (int i = 0; i < (int)res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        String mac = String(d->getAddress().toString().c_str());
        mac.toLowerCase();
        if (mac == want) { addr = d->getAddress(); found = true; break; }
    }
    scan->clearResults();
    if (!found) return false;

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(new InvCb(), false);
        s_client->setConnectionParams(24, 40, 0, 200);
        s_client->setConnectTimeout(10);
    }

    Serial.printf("[инв] подключаюсь к донглу %s\n", want.c_str());
    if (!s_client->connect(addr)) return false;

    NimBLERemoteService* svc = s_client->getService(SVC_UUID);
    if (!svc) {
        Serial.println("[инв] нет сервиса 5330…, это не донгл Eybond");
        s_client->disconnect();
        return false;
    }
    NimBLERemoteCharacteristic* rx = svc->getCharacteristic(CHR_NOTIFY);
    s_tx = svc->getCharacteristic(CHR_WRITE);
    if (!rx || !s_tx || !rx->subscribe(true, onNotify)) {
        Serial.println("[инв] характеристики не нашлись или подписка не прошла");
        s_client->disconnect();
        return false;
    }

    Serial.println("[инв] подключено");
    return true;
}

void invTrSend(const uint8_t* frame, size_t n) {
    if (!s_tx || !s_connected) return;
    s_tx->writeValue(frame, n, !s_tx->canWriteNoResponse());
}

void invBind(const String& mac) {
    cfg().invMac = mac;
    cfgSave();
    Serial.printf("[инв] привязан донгл %s\n", mac.c_str());
    if (s_client && s_client->isConnected()) s_client->disconnect();
}

#endif
