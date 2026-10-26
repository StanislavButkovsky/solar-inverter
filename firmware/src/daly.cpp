// Сборки без Bluetooth (NO_BLE: HS35, Asterion) берут daly_none.cpp.
#if !defined(NO_BLE)

#include "daly.h"
#include "state.h"
#include "cfg.h"
#include "events.h"
#include "inv.h"
#include "net.h"
#include "modbus.h"
#include <NimBLEDevice.h>
#include <time.h>
#include <stdarg.h>

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

// Bluetooth поднят (на HS35 — только при привязанной батарее, см. main.cpp).
static bool s_ble = true;
void dalySetBle(bool on) { s_ble = on; }
bool dalyBle() { return s_ble; }
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
            s.socKnown = true;
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
        s.proto       = 1;
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

// Сырые уведомления — в журнал: первые несколько и дальше каждое сотое.
// 26.10.2026 батарея DL-… подключилась и подписалась, но за минуту не дала
// ни одного кадра A5 — непонятно было, молчит она или говорит иначе.
static volatile uint32_t s_notifies = 0;
static uint32_t          s_connAt = 0;

static void logHex(const char* what, const uint8_t* d, size_t n) {
    char h[3 * 24 + 1];
    size_t m = n < 24 ? n : 24, k = 0;
    for (size_t i = 0; i < m; i++) k += snprintf(h + k, sizeof(h) - k, "%02X ", d[i]);
    h[k] = 0;
    bleLog("[ble] %s, %u байт: %s%s", what, (unsigned)n, h, n > m ? "…" : "");
}

// Ответ протокола D2 (Modbus): D2 03 <байт> <регистры…> <CRC>. Собираем
// целиком — он может прийти несколькими уведомлениями — и держим последний
// для разбора: карта регистров этой BMS ещё не сверена.
static uint8_t  s_d2[160];
static size_t   s_d2Len = 0;
static uint16_t s_d2Reg[80];
static uint8_t  s_d2N = 0;
static uint32_t s_d2Ms = 0;

// Карта D2 (новые DALY под приложением Smart BMS 4.x), сверена 26.10.2026 на
// батарее DL-41190401535D (14S) с инвертором: 56,0 В против 55,9 у инвертора,
// ток со знаком заряда, ячейки 4002…4007 мВ, разброс 5 мВ.
//   0–31 ячейки, мВ · 32–39 температуры +40 (255 — нет датчика)
//   40 напряжение ×0,1 В · 41 ток (x−30000)×0,1 А, + заряд · 42 SOC ×0,1 %
//   43/44 макс./мин. ячейка, мВ · 45/46 макс./мин. температура +40
//   47 состояние (0 покой, 1 заряд, 2 разряд) · 48 остаток ×0,1 А·ч
//   49 число ячеек · 50 число датчиков · 51 циклы · 53/54 ключи заряда/разряда
//   57 мощность, Вт? · 59–61 аварии — не расшифрованы, пишутся в журнал
static void applyD2() {
    const uint16_t* r = s_d2Reg;
    if (s_d2N < 58) return;
    uint8_t nc = r[49] > 0 && r[49] <= 32 ? r[49] : 0;
    uint8_t nt = r[50] <= 8 ? r[50] : 0;
    stateUpdate([&](BmsState& s) {
        s.proto     = 2;
        s.cellCount = nc;
        s.tempCount = nt;
        for (uint8_t i = 0; i < nc && i < MAX_CELLS; i++) s.cellMv[i] = r[i];
        for (uint8_t i = 0; i < nt && i < MAX_TEMPS; i++) s.cellTemp[i] = r[32 + i] == 255 ? 0 : (int16_t)r[32 + i] - 40;
        s.voltage   = r[40] * 0.1f;
        s.current   = ((int32_t)r[41] - 30000) * 0.1f;
        s.soc       = r[42] * 0.1f;
        s.socKnown  = true;
        s.cellMaxMv = r[43];
        s.cellMinMv = r[44];
        s.tempMax   = (int16_t)r[45] - 40;
        s.tempMin   = (int16_t)r[46] - 40;
        s.chargeState  = r[47] <= 2 ? r[47] : 0;
        s.remainingMah = (uint32_t)r[48] * 100;
        if (s.soc > 5) s.capAh = r[48] * 0.1f * 100.0f / s.soc;
        s.cycles       = r[51];
        s.chargeMos    = r[53] != 0;
        s.dischargeMos = r[54] != 0;
        s.framesOk++;
        s.lastFrameMs = millis();
        s.fresh = true;
    });
    static uint16_t lastF[3] = {0, 0, 0};
    if (s_d2N >= 62 && (r[59] != lastF[0] || r[60] != lastF[1] || r[61] != lastF[2])) {
        lastF[0] = r[59]; lastF[1] = r[60]; lastF[2] = r[61];
        bleLog("[ble] D2 аварии/флаги 59–61: %04X %04X %04X", r[59], r[60], r[61]);
    }
}

static void feedD2(const uint8_t* p, size_t n) {
    if (s_d2Len == 0 && !(n >= 3 && p[0] == 0xD2 && p[1] == 0x03)) return;
    if (s_d2Len + n > sizeof(s_d2)) { s_d2Len = 0; return; }
    memcpy(s_d2 + s_d2Len, p, n);
    s_d2Len += n;
    size_t need = 3 + (size_t)s_d2[2] + 2;
    if (s_d2Len < need) return;
    if (mbCrc16(s_d2, need - 2) == (uint16_t)(s_d2[need - 2] | (s_d2[need - 1] << 8))) {
        s_d2N = s_d2[2] / 2 > 80 ? 80 : s_d2[2] / 2;
        for (uint8_t i = 0; i < s_d2N; i++) s_d2Reg[i] = (s_d2[3 + i * 2] << 8) | s_d2[4 + i * 2];
        s_d2Ms = millis();
        applyD2();
    } else {
        bleLog("[ble] ответ D2 с неверной суммой, %u байт", (unsigned)s_d2Len);
    }
    s_d2Len = 0;
}

String dalyD2Report() {
    if (!s_d2N) return String();
    String o = "регистры D2 (номер: число), ответ " + String((millis() - s_d2Ms) / 1000) + " с назад:\n";
    for (uint8_t i = 0; i < s_d2N; i++) {
        char b[24];
        snprintf(b, sizeof(b), "%2u: %5u%s", i, s_d2Reg[i], (i % 8 == 7 || i == s_d2N - 1) ? "\n" : "   ");
        o += b;
    }
    return o;
}

static void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    uint32_t c = ++s_notifies;
    if (c <= 6 || c % 100 == 0) logHex(("уведомление " + String(c)).c_str(), data, len);
    feedD2(data, len);
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
        bleLog("[ble] соединение потеряно");
        evAdd(EV_BMS_DOWN);
    }
};

// Кольцо — статическое, не в куче: 26.10.2026 кольцо из String съело ~12 КБ
// и обновление по сети упало на нехватке памяти (минимум свободной — 4 КБ).
static const uint8_t BLE_LOG_N = 40, BLE_LOG_W = 176;   // русская буква — 2 байта
static char          s_log[BLE_LOG_N][BLE_LOG_W];
static uint8_t       s_logHead = 0, s_logCnt = 0;
static portMUX_TYPE  s_logMux = portMUX_INITIALIZER_UNLOCKED;

void bleLog(const char* fmt, ...) {
    char b[BLE_LOG_W - 10];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    Serial.println(b);
    char t[10];
    time_t now = time(nullptr);
    if (now > 1700000000) {                       // время есть — местное
        now += 3 * 3600;
        struct tm lt;
        gmtime_r(&now, &lt);
        snprintf(t, sizeof(t), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    } else {
        snprintf(t, sizeof(t), "+%lus", (unsigned long)(millis() / 1000));
    }
    char line[BLE_LOG_W];
    snprintf(line, sizeof(line), "%s %s", t, b);
    portENTER_CRITICAL(&s_logMux);
    memcpy(s_log[s_logHead], line, BLE_LOG_W);
    s_logHead = (s_logHead + 1) % BLE_LOG_N;
    if (s_logCnt < BLE_LOG_N) s_logCnt++;
    portEXIT_CRITICAL(&s_logMux);
}

String bleLogText() {
    String out;
    out.reserve(BLE_LOG_N * 70);
    char line[BLE_LOG_W];
    for (uint8_t i = 0; i < BLE_LOG_N; i++) {
        portENTER_CRITICAL(&s_logMux);
        bool have = i < s_logCnt;
        if (have) memcpy(line, s_log[(s_logHead + BLE_LOG_N - s_logCnt + i) % BLE_LOG_N], BLE_LOG_W);
        portEXIT_CRITICAL(&s_logMux);
        if (!have) break;
        line[BLE_LOG_W - 1] = 0;
        out += line;
        out += "\n";
    }
    return out;
}

// Итог поиска одной строкой: сколько услышали и кого громче всех. «Слышим
// одно устройство на весь дом» и «слышим двадцать, но не батарею» — разные беды.
static void logScan(const char* what, const NimBLEScanResults& res, int hit) {
    String top;
    int n = res.getCount();
    bool used[64] = {false};
    for (int k = 0; k < 3 && k < n; k++) {
        int best = -1;
        for (int i = 0; i < n && i < 64; i++)
            if (!used[i] && (best < 0 || res.getDevice(i)->getRSSI() > res.getDevice(best)->getRSSI())) best = i;
        if (best < 0) break;
        used[best] = true;
        const NimBLEAdvertisedDevice* d = res.getDevice(best);
        String nm = String(d->getName().c_str());
        if (nm.length() > 14) nm = nm.substring(0, 14);
        top += String(k ? ", " : "") + (nm.length() ? nm : String(d->getAddress().toString().c_str())) +
               " " + String(d->getRSSI());
    }
    bleLog("[ble] %s: услышано %d%s%s", what, n,
           hit >= 0 ? ", нужное найдено" : ", нужного нет",
           n ? (String("; громче всех: ") + top).c_str() : "");
}

static bool looksLikeDaly(const NimBLEAdvertisedDevice* d) {
    if (d->isAdvertisingService(SVC_UUID)) return true;
    String n = String(d->getName().c_str());
    n.toUpperCase();
    return n.startsWith("DL-") || n.startsWith("DALY") || n.startsWith("SMART BMS");
}

std::vector<BleFound> dalyScan(uint8_t seconds) {
    std::vector<BleFound> out;
    if (!s_ble) return out;                      // Bluetooth не поднят — искать нечем
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    // Радио у ESP32 одно на Wi-Fi и Bluetooth. С окном 99 из 100 точка доступа
    // 26.10.2026 не пускала телефон вовсе: «Не удаётся подключиться», до платы
    // не доходило ни одной попытки. Пока она поднята — человек настраивает
    // модуль — эфир делим: Bluetooth слушает четверть времени.
    scan->setWindow(netApActive() ? 25 : 99);
    NimBLEScanResults res = scan->getResults(seconds * 1000, false);
    logScan("поиск по запросу", res, -1);
    for (int i = 0; i < (int)res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        BleFound f;
        f.mac  = String(d->getAddress().toString().c_str());
        f.name = String(d->getName().c_str());
        f.rssi = d->getRSSI();
        f.hasDalyService = looksLikeDaly(d);
        f.hasEybond = d->isAdvertisingService(NimBLEUUID("53300000-0023-4bd4-bbd5-a6920e4c5653"));
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
    scan->setWindow(netApActive() ? 25 : 99);     // см. dalyScan()
    NimBLEScanResults res = scan->getResults(seconds * 1000, false);

    s_haveTarget = false;
    int hitIdx = -1;
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
        hitIdx = i;
        break;
    }
    logScan(want.length() ? "поиск батареи по адресу" : "поиск батареи DALY", res, hitIdx);
    scan->clearResults();
    return s_haveTarget;
}

static bool connectTarget() {
    if (!s_haveTarget) return false;

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(new ClientCB(), false);
        s_client->setConnectionParams(24, 40, 0, 200);
        // NimBLE 2.x — миллисекунды (в 1.x были секунды): «10» давало 10 мс,
        // и подключение 26.10.2026 обрывалось таймаутом (код 13) мгновенно.
        s_client->setConnectTimeout(10000);
    }

    bleLog("[ble] подключаюсь к %s (%s)",
                  s_targetAddr.toString().c_str(), s_targetName.c_str());

    if (!s_client->connect(s_targetAddr)) {
        bleLog("[ble] подключиться не удалось, код NimBLE %d", s_client->getLastError());
        return false;
    }

    NimBLERemoteService* svc = s_client->getService(SVC_UUID);
    if (!svc) {
        bleLog("[ble] нет сервиса FFF0 — это не DALY либо другой донгл");
        s_client->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic* rx = svc->getCharacteristic(CHR_NOTIFY);
    s_txChar = svc->getCharacteristic(CHR_WRITE);
    if (!rx || !s_txChar) {
        bleLog("[ble] нет характеристик FFF1/FFF2");
        s_client->disconnect();
        return false;
    }
    if (!rx->subscribe(true, notifyCB)) {
        bleLog("[ble] подписка на уведомления не прошла");
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

    bleLog("[ble] подключено, подписка активна; запись %s",
           s_txChar->canWriteNoResponse() ? "без подтверждения" : "с подтверждением");
    s_notifies = 0;
    s_connAt = millis();
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
    bleLog("[ble] привязана батарея %s", mac.c_str());
    if (!s_ble) bleLog("[ble] Bluetooth на этой загрузке выключен — включится после перезагрузки");
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
    invStart();

    for (;;) {
        // Инвертор обслуживается из этой же задачи: все обращения к стеку BLE
        // идут из одного контекста, и два соединения не мешают друг другу.
        invTick();

        // Поиск по запросу из веба — раньше всего остального: им ищут батарею,
        // когда связи ещё нет.
        if (s_scanReq) {
            s_scanReq  = false;
            s_scanBusy = true;
            bleLog("[ble] поиск по запросу из веба");
            std::vector<BleFound> found = dalyScan(5);
            if (s_scanMtx && xSemaphoreTake(s_scanMtx, pdMS_TO_TICKS(500)) == pdTRUE) {
                s_scanRes = found;
                xSemaphoreGive(s_scanMtx);
            }
            s_scanBusy = false;
            bleLog("[ble] найдено устройств: %u", (unsigned)found.size());
            continue;
        }

        // Пауза по требованию: батарея отдана официальному приложению.
        if (s_releaseUntil && (int32_t)(millis() - s_releaseUntil) < 0) {
            if (s_client && s_client->isConnected()) {
                bleLog("[ble] отпускаю батарею приложению");
                s_client->disconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_releaseUntil && (int32_t)(millis() - s_releaseUntil) >= 0) {
            s_releaseUntil = 0;
            bleLog("[ble] пауза кончилась, возвращаюсь к опросу");
        }

        if (!s_connected) {
#if defined(TARGET_HS35)
            // У HS35 батарея штатная и говорит через инвертор; своей BMS с
            // Bluetooth может не быть вовсе. Искать её вслепую — значит каждые
            // 5 с на 5 с занимать и радио, и эту задачу, из которой опрашивается
            // инвертор: на живой плате опрос шёл раз в 5 с вместо 2, а Wi-Fi
            // падал на совместной работе радио. Ищем, только если привязана.
            if (!s_ble || cfg().bmsMac.isEmpty()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
#endif
            // При поднятой точке доступа — реже: см. окно в dalyScan().
            if (millis() - lastTry > (netApActive() ? 20000u : 5000u)) {
                lastTry = millis();
                if (findTarget(5)) {
                    if (!connectTarget()) vTaskDelay(pdMS_TO_TICKS(2000));
                } else {
                    // итог поиска уже в журнале (logScan)
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Протокол: старый DALY (кадры A5 по DataID) или новый — Modbus с
        // адресом D2, все 62 регистра одним запросом (BMS под приложением
        // Smart BMS 4.x). Пока не ясно — A5 по кругу и раз в 10 с проба D2.
        uint8_t proto = stateSnapshot().proto;
        static uint32_t lastD2 = 0;
        if (proto != 2) {
            requestId(cycle[step]);
            step = (step + 1) % (sizeof(cycle) / sizeof(cycle[0]));
        }
        uint32_t d2Every = proto == 2 ? 2000 : 10000;
        if (proto != 1 && s_txChar && millis() - s_connAt > (proto == 2 ? 0u : 15000u) && millis() - lastD2 > d2Every) {
            lastD2 = millis();
            uint8_t q[8];
            mbBuildRead(q, 0xD2, 0x0000, 0x3E);
            if (proto != 2) logHex("молчит на A5, пробую D2", q, 8);
            s_txChar->writeValue(q, 8, !s_txChar->canWriteNoResponse());
        }

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

#endif
