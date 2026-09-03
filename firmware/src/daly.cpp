#include "daly.h"
#include "state.h"
#include "cfg.h"
#include "events.h"
#include <NimBLEDevice.h>

static const NimBLEUUID SVC_UUID((uint16_t)0xFFF0);
static const NimBLEUUID CHR_NOTIFY((uint16_t)0xFFF1);
static const NimBLEUUID CHR_WRITE((uint16_t)0xFFF2);

// Данные считаются свежими, если валидный кадр приходил не позже этого срока.
static const uint32_t STALE_MS = 15000;

static NimBLEClient*               s_client   = nullptr;
static NimBLERemoteCharacteristic* s_txChar   = nullptr;
static volatile bool               s_connected = false;
static volatile uint32_t           s_releaseUntil = 0;   // millis(), 0 = не отпущено

// Цель храним адресом, а не копией объекта: результаты сканирования живут
// до ближайшего clearResults(), а адрес самодостаточен.
// Асинхронный поиск: веб только просит, сканирует задача BLE — все операции
// стека должны идти из одного контекста.
static volatile bool  s_scanReq  = false;
static volatile bool  s_scanBusy = false;
static std::vector<BleFound> s_scanRes;
static SemaphoreHandle_t     s_scanMtx = nullptr;

static NimBLEAddress s_targetAddr;
static String        s_targetName;
static int           s_targetRssi = 0;
static bool          s_haveTarget = false;

// ---------------------------------------------------------------- протокол

static uint8_t checksum(const uint8_t* f, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += f[i];
    return (uint8_t)(sum & 0xFF);
}

static void buildRequest(uint8_t dataId, uint8_t* out) {
    out[0] = 0xA5;
    out[1] = DALY_ADDR_HOST;
    out[2] = dataId;
    out[3] = 0x08;
    memset(out + 4, 0x00, 8);
    out[12] = checksum(out, 12);
}

static inline uint16_t be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void dalyParseFrame(const uint8_t* f) {
    const uint8_t  id = f[2];
    const uint8_t* d  = f + 4;   // восемь байт полезной нагрузки

    stateUpdate([id, d](BmsState& s) {
        switch (id) {

        case 0x90:  // суммарное напряжение, ток, SOC
            s.voltage = be16(d + 0) * 0.1f;
            s.current = ((int32_t)be16(d + 4) - 30000) * 0.1f;
            s.soc     = be16(d + 6) * 0.1f;
            break;

        case 0x91:  // максимальная и минимальная ячейка
            s.cellMaxMv = be16(d + 0);
            s.cellMaxNo = d[2];
            s.cellMinMv = be16(d + 3);
            s.cellMinNo = d[5];
            break;

        case 0x92:  // температуры, смещение 40
            s.tempMax   = (int16_t)d[0] - 40;
            s.tempMaxNo = d[1];
            s.tempMin   = (int16_t)d[2] - 40;
            s.tempMinNo = d[3];
            break;

        case 0x93:  // состояние, ключи, остаточная ёмкость
            s.chargeState  = d[0];
            s.chargeMos    = d[1] != 0;
            s.dischargeMos = d[2] != 0;
            s.bmsLife      = d[3];
            s.remainingMah = be32(d + 4);
            break;

        case 0x94:  // конфигурация
            s.cellCount = d[0];
            s.tempCount = d[1];
            s.chargerOn = d[2];
            s.loadOn    = d[3];
            break;

        case 0x95: {  // напряжения ячеек, серия кадров по три ячейки
            uint8_t frameNo = d[0];               // нумерация с единицы
            if (frameNo == 0 || frameNo > 16) break;
            for (uint8_t k = 0; k < 3; k++) {
                uint16_t idx = (uint16_t)(frameNo - 1) * 3 + k;
                if (idx >= MAX_CELLS) break;
                uint16_t mv = be16(d + 1 + k * 2);
                // хвост последнего кадра добит нулями — не затираем реальные ячейки
                if (mv == 0 && idx >= s.cellCount && s.cellCount) continue;
                s.cellMv[idx] = mv;
            }
            break;
        }

        case 0x96: {  // температуры ячеек, серия кадров по семь значений
            uint8_t frameNo = d[0];
            if (frameNo == 0) break;
            for (uint8_t k = 0; k < 7; k++) {
                uint16_t idx = (uint16_t)(frameNo - 1) * 7 + k;
                if (idx >= MAX_TEMPS) break;
                s.cellTemp[idx] = (int16_t)d[1 + k] - 40;
            }
            break;
        }

        case 0x97:  // балансировка, по биту на ячейку
            memcpy(s.balance, d, 6);
            break;

        case 0x98:  // флаги аварий
            memcpy(s.faults, d, 7);
            s.faults[7]   = 0;
            s.haveFaults  = true;
            break;

        default:
            break;
        }

        s.framesOk++;
        s.lastFrameMs = millis();
        s.fresh       = true;
    });
}

// ------------------------------------------------------- приём и сборка потока

// Уведомление может нести один кадр или несколько подряд (так приходит 0x95),
// а может прийти рваным. Собираем поток и синхронизируемся по 0xA5 + контрольной сумме.
static uint8_t s_buf[64];
static size_t  s_len = 0;

static void feed(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s_len == 0 && data[i] != 0xA5) continue;      // ждём начало кадра
        if (s_len < sizeof(s_buf)) s_buf[s_len++] = data[i];

        if (s_len == DALY_FRAME_LEN) {
            if (checksum(s_buf, 12) == s_buf[12] && s_buf[1] == DALY_ADDR_BMS) {
                dalyParseFrame(s_buf);
            } else {
                stateUpdate([](BmsState& s) { s.framesBad++; });
                // рассинхронизация: сдвигаемся на байт и пробуем ещё раз
                memmove(s_buf, s_buf + 1, DALY_FRAME_LEN - 1);
                s_len = DALY_FRAME_LEN - 1;
                // подрезаем до ближайшего 0xA5, иначе будем жевать мусор
                size_t k = 0;
                while (k < s_len && s_buf[k] != 0xA5) k++;
                if (k) { memmove(s_buf, s_buf + k, s_len - k); s_len -= k; }
                continue;
            }
            s_len = 0;
        }
    }
}

static void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    feed(data, len);
}

// ------------------------------------------------------------------ соединение

class ClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        s_connected = true;
    }
    void onDisconnect(NimBLEClient* c, int reason) override {
        s_connected = false;
        s_txChar    = nullptr;
        s_len       = 0;
        stateUpdate([](BmsState& s) {
            s.linked = false;
            s.fresh  = false;
            s.disconnects++;
        });
        Serial.println("[ble] соединение потеряно");
        evAdd(EV_BMS_DOWN);
    }
};

static bool looksLikeDaly(const NimBLEAdvertisedDevice* d) {
    if (d->isAdvertisingService(SVC_UUID)) return true;
    String n = String(d->getName().c_str());
    n.toUpperCase();
    return n.startsWith("DL-") || n.startsWith("DALY") || n.startsWith("SMART BMS");
}

std::vector<BleFound> dalyScan(uint8_t seconds) {
    std::vector<BleFound> out;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults res = scan->getResults(seconds * 1000, false);
    for (int i = 0; i < (int)res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        BleFound f;
        f.mac  = String(d->getAddress().toString().c_str());
        f.name = String(d->getName().c_str());
        f.rssi = d->getRSSI();
        f.hasDalyService = looksLikeDaly(d);
        out.push_back(f);
    }
    scan->clearResults();
    return out;
}

// Ищем цель: по MAC из конфига, иначе первое устройство, похожее на DALY.
static bool findTarget(uint8_t seconds) {
    String want = cfg().bmsMac;
    want.toLowerCase();

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults res = scan->getResults(seconds * 1000, false);

    s_haveTarget = false;
    for (int i = 0; i < (int)res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        String mac = String(d->getAddress().toString().c_str());
        mac.toLowerCase();
        bool hit = want.length() ? (mac == want) : looksLikeDaly(d);
        if (!hit) continue;
        s_targetAddr = d->getAddress();
        s_targetName = String(d->getName().c_str());
        s_targetRssi = d->getRSSI();
        s_haveTarget = true;
        break;
    }
    scan->clearResults();
    return s_haveTarget;
}

static bool connectTarget() {
    if (!s_haveTarget) return false;

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(new ClientCB(), false);
        s_client->setConnectionParams(24, 40, 0, 200);
        s_client->setConnectTimeout(10);
    }

    Serial.printf("[ble] подключаюсь к %s (%s)\n",
                  s_targetAddr.toString().c_str(), s_targetName.c_str());

    if (!s_client->connect(s_targetAddr)) {
        Serial.println("[ble] подключиться не удалось");
        return false;
    }

    NimBLERemoteService* svc = s_client->getService(SVC_UUID);
    if (!svc) {
        Serial.println("[ble] нет сервиса FFF0 — это не DALY либо другой донгл");
        s_client->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic* rx = svc->getCharacteristic(CHR_NOTIFY);
    s_txChar = svc->getCharacteristic(CHR_WRITE);
    if (!rx || !s_txChar) {
        Serial.println("[ble] нет характеристик FFF1/FFF2");
        s_client->disconnect();
        return false;
    }
    if (!rx->subscribe(true, notifyCB)) {
        Serial.println("[ble] подписка на уведомления не прошла");
        s_client->disconnect();
        return false;
    }

    String mac  = String(s_targetAddr.toString().c_str());
    int8_t rssi = (int8_t)s_targetRssi;
    stateUpdate([&mac, rssi](BmsState& s) {
        s.linked = true;
        strncpy(s.mac, mac.c_str(), sizeof(s.mac) - 1);
        s.mac[sizeof(s.mac) - 1] = 0;
        s.rssi = rssi;
    });

    Serial.println("[ble] подключено, подписка активна");
    evAdd(EV_BMS_UP, s_targetAddr.toString().c_str());
    return true;
}

static void requestId(uint8_t dataId) {
    if (!s_txChar || !s_connected) return;
    uint8_t req[DALY_FRAME_LEN];
    buildRequest(dataId, req);
    // Часть донглов принимает только запись без подтверждения — выбираем по факту.
    bool noResp = s_txChar->canWriteNoResponse();
    s_txChar->writeValue(req, DALY_FRAME_LEN, !noResp);
}

void dalyRequestScan() { s_scanReq = true; }
bool dalyScanBusy()     { return s_scanBusy || s_scanReq; }

std::vector<BleFound> dalyScanResults() {
    std::vector<BleFound> copy;
    if (s_scanMtx && xSemaphoreTake(s_scanMtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        copy = s_scanRes;
        xSemaphoreGive(s_scanMtx);
    }
    return copy;
}

void dalyBind(const String& mac) {
    cfg().bmsMac = mac;
    cfgSave();
    Serial.printf("[ble] привязана батарея %s\n", mac.c_str());
    if (s_client && s_client->isConnected()) s_client->disconnect();
    s_haveTarget = false;
}

// ------------------------------------------------------------------ задача

static void dalyTask(void*) {
    // NimBLEDevice::init() уже вызван в setup(): стек общий с периферийной ролью.

    // 0x94 и 0x96 достаточно опрашивать редко: конфигурация не меняется,
    // а температур у типовой сборки всего одна-две.
    static const uint8_t cycle[] = {0x90, 0x91, 0x92, 0x93, 0x95, 0x98, 0x90, 0x91, 0x97, 0x94, 0x96};
    uint8_t step = 0;
    uint32_t lastTry = 0;

    s_scanMtx = xSemaphoreCreateMutex();

    for (;;) {
        // Поиск по запросу из веба — раньше всего остального: им ищут батарею,
        // когда связи ещё нет.
        if (s_scanReq) {
            s_scanReq  = false;
            s_scanBusy = true;
            Serial.println("[ble] поиск по запросу из веба");
            std::vector<BleFound> found = dalyScan(5);
            if (s_scanMtx && xSemaphoreTake(s_scanMtx, pdMS_TO_TICKS(500)) == pdTRUE) {
                s_scanRes = found;
                xSemaphoreGive(s_scanMtx);
            }
            s_scanBusy = false;
            Serial.printf("[ble] найдено устройств: %u\n", (unsigned)found.size());
            continue;
        }

        // Пауза по требованию: батарея отдана официальному приложению.
        if (s_releaseUntil && (int32_t)(millis() - s_releaseUntil) < 0) {
            if (s_client && s_client->isConnected()) {
                Serial.println("[ble] отпускаю батарею приложению");
                s_client->disconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_releaseUntil && (int32_t)(millis() - s_releaseUntil) >= 0) {
            s_releaseUntil = 0;
            Serial.println("[ble] пауза кончилась, возвращаюсь к опросу");
        }

        if (!s_connected) {
            if (millis() - lastTry > 5000) {
                lastTry = millis();
                if (findTarget(5)) {
                    if (!connectTarget()) vTaskDelay(pdMS_TO_TICKS(2000));
                } else {
                    Serial.println("[ble] батарея не найдена в эфире");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        requestId(cycle[step]);
        step = (step + 1) % (sizeof(cycle) / sizeof(cycle[0]));

        // Свежесть данных: молчание дольше STALE_MS — это уже не связь.
        stateUpdate([](BmsState& s) {
            if (s.lastFrameMs && millis() - s.lastFrameMs > STALE_MS) s.fresh = false;
        });

        vTaskDelay(pdMS_TO_TICKS(cfg().pollMs));
    }
}

void dalyStart() {
    xTaskCreatePinnedToCore(dalyTask, "daly", 8192, nullptr, 5, nullptr, 0);
}

void dalyReleaseFor(uint32_t minutes) {
    s_releaseUntil = millis() + minutes * 60000UL;
    if (s_releaseUntil == 0) s_releaseUntil = 1;   // 0 зарезервирован под «не отпущено»
}

bool dalyIsReleased() {
    return s_releaseUntil && (int32_t)(millis() - s_releaseUntil) < 0;
}

uint32_t dalyReleaseLeftS() {
    if (!dalyIsReleased()) return 0;
    return (s_releaseUntil - millis()) / 1000;
}
