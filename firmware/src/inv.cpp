#include "inv.h"
#include "state.h"
#include "cfg.h"
#include "events.h"
#include <NimBLEDevice.h>

static const NimBLEUUID SVC_UUID("53300000-0023-4bd4-bbd5-a6920e4c5653");
static const NimBLEUUID CHR_WRITE("53300001-0023-4bd4-bbd5-a6920e4c5653");
static const NimBLEUUID CHR_NOTIFY("53300005-0023-4bd4-bbd5-a6920e4c5653");

static const uint8_t  SLAVE   = 1;
static const uint8_t  FUNC_RD = 0x03;
static const uint32_t STALE_MS = 20000;
static const uint32_t POLL_MS  = 2000;

// Два блока регистров вместо одного: длинный ответ дробится на уведомления,
// и чем короче кадр, тем меньше шансов потерять хвост.
struct Block { uint16_t start, count; };
static const Block BLOCKS[] = { {201, 21}, {231, 3} };
static const uint8_t NBLOCKS = sizeof(BLOCKS) / sizeof(BLOCKS[0]);

static NimBLEClient*               s_client = nullptr;
static NimBLERemoteCharacteristic* s_tx     = nullptr;
static volatile bool               s_connected = false;
static uint8_t                     s_block  = 0;
static uint32_t                    s_lastPoll = 0, s_lastTry = 0;

// ------------------------------------------------------------------ протокол

static uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static inline uint16_t be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline int16_t  s16(const uint8_t* p)  { return (int16_t)be16(p); }

static void parseBlock(uint16_t start, const uint8_t* d, uint8_t words) {
    // Регистр N лежит по смещению (N - start) слов.
    auto R  = [&](uint16_t n) -> uint16_t {
        uint16_t i = n - start;
        return (i < words) ? be16(d + i * 2) : 0;
    };
    auto RS = [&](uint16_t n) -> int16_t {
        uint16_t i = n - start;
        return (i < words) ? s16(d + i * 2) : 0;
    };

    invUpdate([&](InvState& v) {
        v.linked      = true;
        v.fresh       = true;
        v.lastFrameMs = millis();

        if (start == 201) {
            uint16_t mode = R(201);
            v.gridV   = R(202) * 0.1f;
            v.gridHz  = R(203) * 0.01f;
            v.outV    = R(206) * 0.1f;
            v.outHz   = R(208) * 0.01f;
            v.outW    = R(209) * 1.0f;      // регистр хранит ватты в единицах 0.001 кВт
            v.loadPct = (uint8_t)R(214);
            v.pvV     = R(211) * 0.1f;
            v.pvW     = R(213) * 1.0f;

            float battW = RS(218) * 1.0f;   // знак: минус — разряд
            v.chgW = battW > 0 ? battW : 0;

            v.tempC = RS(220);              // температура самого инвертора

            // Режим уточняется битами регистра 231, здесь только грубо.
            if (mode == 3)      v.mode = INVM_BATTERY;
            else if (mode == 1) v.mode = INVM_LINE;
            else                v.mode = INVM_STANDBY;
        } else if (start == 231) {
            uint16_t st = R(231);
            v.gridOn    = (st >> 2) & 1;
            v.faultCode = (uint8_t)R(232);
            if (v.faultCode) v.mode = INVM_FAULT;
            // Приоритеты источников в этой карте не представлены.
            v.prioOut = 0xFF;
            v.prioChg = 0xFF;
        }
    });
}

// ------------------------------------------------------- приём и склейка

static uint8_t s_buf[300];
static size_t  s_len = 0;

static void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t n, bool) {
    if (s_len + n > sizeof(s_buf)) s_len = 0;         // потеряли начало — сброс
    memcpy(s_buf + s_len, data, n);
    s_len += n;

    // Ответ начинается с восьми ASCII-единиц. Ждём их и заголовок Modbus.
    if (s_len < 8 + 3) return;
    for (uint8_t i = 0; i < 8; i++)
        if (s_buf[i] != '1') { s_len = 0; return; }

    const uint8_t* f = s_buf + 8;
    size_t avail = s_len - 8;
    if (f[0] != SLAVE || f[1] != FUNC_RD) { s_len = 0; return; }

    uint8_t bytes = f[2];
    size_t  need  = 3 + bytes + 2;
    if (avail < need) return;                         // хвост ещё не пришёл

    uint16_t got  = f[need - 2] | (f[need - 1] << 8); // CRC идёт младшим байтом
    if (got == crc16(f, need - 2)) {
        parseBlock(BLOCKS[s_block].start, f + 3, bytes / 2);
        s_block = (s_block + 1) % NBLOCKS;
    } else {
        Serial.println("[инв] контрольная сумма не сошлась");
    }
    s_len = 0;
}

// ------------------------------------------------------------------ связь

class InvCb : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override { s_connected = true; }
    void onDisconnect(NimBLEClient*, int) override {
        s_connected = false;
        s_tx = nullptr;
        s_len = 0;
        invUpdate([](InvState& v) { v.linked = false; v.fresh = false; });
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

static bool connectDongle() {
    String want = cfg().invMac;
    want.toLowerCase();
    if (!want.length()) return false;

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
    evAdd(EV_INV_UP, want.c_str());
    return true;
}

static void request(const Block& b) {
    if (!s_tx || !s_connected) return;
    uint8_t f[8] = { SLAVE, FUNC_RD,
                     (uint8_t)(b.start >> 8), (uint8_t)(b.start & 0xFF),
                     (uint8_t)(b.count >> 8), (uint8_t)(b.count & 0xFF), 0, 0 };
    uint16_t c = crc16(f, 6);
    f[6] = c & 0xFF;                 // CRC младшим байтом вперёд
    f[7] = c >> 8;
    s_len = 0;
    s_tx->writeValue(f, sizeof(f), !s_tx->canWriteNoResponse());
}

// ------------------------------------------------------------------ шаг

void invBind(const String& mac) {
    cfg().invMac = mac;
    cfgSave();
    Serial.printf("[инв] привязан донгл %s\n", mac.c_str());
    if (s_client && s_client->isConnected()) s_client->disconnect();
    s_lastTry = 0;
}

void invStart() {
    invUpdate([](InvState& v) { v = InvState(); });
}

void invTick() {
    if (cfg().invMac.isEmpty()) return;      // донгл не привязан — нечего делать

    if (!s_connected) {
        if (millis() - s_lastTry > 8000) {
            s_lastTry = millis();
            connectDongle();
        }
        return;
    }

    if (millis() - s_lastPoll < POLL_MS) return;
    s_lastPoll = millis();
    request(BLOCKS[s_block]);

    invUpdate([](InvState& v) {
        if (v.lastFrameMs && millis() - v.lastFrameMs > STALE_MS) v.fresh = false;
    });
}
