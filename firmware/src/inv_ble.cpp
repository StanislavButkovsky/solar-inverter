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
#include "daly.h"

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
        bleLog("[инв] соединение потеряно");
    }
};

String invTrReport() { return String(); }   // по BLE всё видно вживую на странице

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
    int rssi = 0;
    bool conn = false;
    for (int i = 0; i < (int)res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        String mac = String(d->getAddress().toString().c_str());
        mac.toLowerCase();
        if (mac == want) { addr = d->getAddress(); rssi = d->getRSSI(); conn = d->isConnectable(); found = true; break; }
    }
    int heard = res.getCount();
    scan->clearResults();
    if (!found) { bleLog("[инв] донгл %s в эфире не найден (услышано %d)", want.c_str(), heard); return false; }

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(new InvCb(), false);
        s_client->setConnectionParams(24, 40, 0, 200);
        // NimBLE 2.x — миллисекунды (в 1.x были секунды): «10» давало 10 мс,
        // и подключение 26.10.2026 обрывалось таймаутом (код 13) мгновенно.
        s_client->setConnectTimeout(10000);
    }

    bleLog("[инв] подключаюсь к донглу %s (%d дБм, %s)", want.c_str(), rssi,
           conn ? "принимает подключения" : "реклама без подключений — слот, видимо, занят");
    if (!s_client->connect(addr)) {
        bleLog("[инв] подключиться не удалось, код NimBLE %d", s_client->getLastError());
        return false;
    }

    NimBLERemoteService* svc = s_client->getService(SVC_UUID);
    if (!svc) {
        String list;
        for (NimBLERemoteService* x : s_client->getServices(true)) list += String(x->getUUID().toString().c_str()) + " ";
        bleLog("[инв] нет сервиса 5330…, это не донгл Eybond; сервисы: %s", list.c_str());
        s_client->disconnect();
        return false;
    }
    NimBLERemoteCharacteristic* rx = svc->getCharacteristic(CHR_NOTIFY);
    s_tx = svc->getCharacteristic(CHR_WRITE);
    if (!rx || !s_tx || !rx->subscribe(true, onNotify)) {
        bleLog("[инв] характеристики не нашлись или подписка не прошла");
        s_client->disconnect();
        return false;
    }

    bleLog("[инв] подключено");
    return true;
}

void invTrSend(const uint8_t* frame, size_t n) {
    if (!s_tx || !s_connected) return;
    s_tx->writeValue(frame, n, !s_tx->canWriteNoResponse());
}

void invBind(const String& mac) {
    cfg().invMac = mac;
    cfgSave();
    bleLog("[инв] привязан донгл %s", mac.c_str());
    if (s_client && s_client->isConnected()) s_client->disconnect();
}

#endif
