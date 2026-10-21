#include "events.h"
#include "json.h"
#include "state.h"
#include <time.h>
#include <LittleFS.h>

static const uint8_t CAP = 40;

struct Event {
    uint32_t   ms;
    time_t     t;         // 0 — время ещё не синхронизировано
    EventKind  kind;
    char       detail[40];   // хватает на «работа от батареи» (33 байта в UTF-8)
    bool       saved;     // уже во флеше
};

// Файл: заголовок и 300 записей кольцом. Запись — 48 байт.
static const char*    F_EV = "/events.bin";
static const uint16_t NFILE = 300;
static const uint32_t EV_MAGIC = 0x31545645;          // "EVT1"
struct __attribute__((packed)) EvHdr { uint32_t magic; uint16_t next, count; };
struct __attribute__((packed)) EvRec { uint32_t t; uint8_t kind; char detail[40]; uint8_t _r[3]; };
static_assert(sizeof(EvRec) == 48, "EvRec");
static bool s_fs = false;

// Обрезка по границе символа. Русский текст в UTF-8 занимает по два байта, и
// обычный strncpy рвал его посередине: получался невалидный JSON, на котором
// страница молча перестаёт обновляться.
static void copyDetail(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strnlen(src, cap - 1);
    while (n > 0 && (src[n] & 0xC0) == 0x80) n--;   // сидим на продолжении — назад
    memcpy(dst, src, n);
    dst[n] = 0;
}

static Event   s_buf[CAP];
static uint8_t s_count = 0;   // сколько всего записано, но не больше CAP
static uint8_t s_head  = 0;   // куда писать следующее

void evAdd(EventKind kind, const char* detail) {
    Event& e = s_buf[s_head];
    e.ms   = millis();
    time_t now = time(nullptr);
    e.t    = (now > 1700000000) ? now : 0;
    e.kind = kind;
    copyDetail(e.detail, sizeof(e.detail), detail);
    e.saved = false;

    s_head = (s_head + 1) % CAP;
    if (s_count < CAP) s_count++;

    Serial.printf("[журнал] %s%s%s\n", evText(kind),
                  e.detail[0] ? ": " : "", e.detail);
}

const char* evText(EventKind k) {
    switch (k) {
    case EV_BOOT:        return "модуль включился";
    case EV_BMS_UP:      return "связь с батареей установлена";
    case EV_BMS_DOWN:    return "связь с батареей потеряна";
    case EV_BMS_STALE:   return "батарея молчит, данные устарели";
    case EV_FAULT_ON:    return "BMS сообщила об аварии";
    case EV_FAULT_OFF:   return "авария BMS снята";
    case EV_CHG_BLOCKED: return "BMS запретила заряд";
    case EV_CHG_OK:      return "заряд снова разрешён";
    case EV_DSG_BLOCKED: return "BMS запретила разряд";
    case EV_DSG_OK:      return "разряд снова разрешён";
    case EV_WIFI_UP:     return "вошёл в домашнюю сеть";
    case EV_WIFI_DOWN:   return "домашняя сеть потеряна";
    case EV_AP_UP:       return "поднята точка доступа";
    case EV_AP_DOWN:     return "точка доступа выключена";
    case EV_GRID_LOST:   return "пропало питание от сети";
    case EV_GRID_BACK:   return "питание от сети вернулось";
    case EV_INV_MODE:    return "инвертор сменил режим";
    case EV_INV_FAULT:   return "авария инвертора";
    case EV_INV_DOWN:    return "связь с инвертором потеряна";
    case EV_INV_UP:      return "связь с инвертором установлена";
    case EV_INV_SET:     return "переключение настройки инвертора";
    }
    return "событие";
}

EventLevel evLevel(EventKind k) {
    switch (k) {
    case EV_FAULT_ON: case EV_INV_FAULT: case EV_GRID_LOST:
    case EV_CHG_BLOCKED: case EV_DSG_BLOCKED:
        return LV_ALARM;
    case EV_BMS_DOWN: case EV_BMS_STALE: case EV_WIFI_DOWN: case EV_INV_DOWN:
        return LV_WARN;
    case EV_BMS_UP: case EV_WIFI_UP: case EV_INV_UP: case EV_GRID_BACK:
    case EV_FAULT_OFF: case EV_CHG_OK: case EV_DSG_OK:
        return LV_GOOD;
    default:
        return LV_INFO;
    }
}

static const char* levelName(EventLevel l) {
    switch (l) {
    case LV_ALARM: return "alarm";
    case LV_WARN:  return "warn";
    case LV_GOOD:  return "good";
    default:       return "info";
    }
}

// ---------------------------------------------------------------- флеш

void evStart() {
    s_fs = true;
    File f = LittleFS.open(F_EV, "r");
    EvHdr h = {0, 0, 0};
    if (f) { f.read((uint8_t*)&h, sizeof(h)); f.close(); }
    if (h.magic == EV_MAGIC && h.next < NFILE && h.count <= NFILE) return;
    f = LittleFS.open(F_EV, "w");                     // нет файла или чужой — завести пустой
    if (!f) { s_fs = false; return; }
    h = { EV_MAGIC, 0, 0 };
    f.write((const uint8_t*)&h, sizeof(h));
    EvRec z;
    memset(&z, 0, sizeof(z));
    for (uint16_t i = 0; i < NFILE; i++) f.write((const uint8_t*)&z, sizeof(z));
    f.close();
}

static void fileAppend(const Event& e, time_t t) {
    File f = LittleFS.open(F_EV, "r+");
    if (!f) return;
    EvHdr h;
    f.read((uint8_t*)&h, sizeof(h));
    EvRec r;
    memset(&r, 0, sizeof(r));
    r.t = (uint32_t)t;
    r.kind = e.kind;
    memcpy(r.detail, e.detail, sizeof(r.detail));
    f.seek(sizeof(EvHdr) + h.next * sizeof(EvRec));
    f.write((const uint8_t*)&r, sizeof(r));
    h.next = (h.next + 1) % NFILE;
    if (h.count < NFILE) h.count++;
    f.seek(0);
    f.write((const uint8_t*)&h, sizeof(h));
    f.close();
}

// Из evWatchLoop: события в памяти — во флеш, старые первыми.
static void flushToFile() {
    if (!s_fs) return;
    time_t now = time(nullptr);
    bool haveTime = now > 1700000000;
    for (uint8_t n = s_count; n > 0; n--) {
        Event& e = s_buf[(s_head + CAP - n) % CAP];
        if (e.saved) continue;
        time_t t = e.t;
        if (!t && haveTime) t = now - (time_t)((millis() - e.ms) / 1000);
        if (!t && millis() < 300000) return;          // ждём время, но не дольше 5 минут
        fileAppend(e, t);
        e.saved = true;
    }
}

// ---------------------------------------------------------------- выдача

static void put(String& j, bool& first, time_t t, uint32_t agoS, EventKind k, const char* detail) {
    char when[24] = {0};
    if (t) {
        struct tm tmv;
        localtime_r(&t, &tmv);
        snprintf(when, sizeof(when), "%02d.%02d %02d:%02d:%02d",
                 tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }
    if (!first) j += ",";
    first = false;
    j += "{\"when\":\"" + String(when) + "\"";
    j += ",\"ago\":" + String(agoS);
    j += ",\"level\":\"" + String(levelName(evLevel(k))) + "\"";
    j += ",\"text\":\"" + String(evText(k)) + "\"";
    j += ",\"detail\":\"" + jsonEsc(String(detail)) + "\"}";
}

String evJson(uint16_t limit) {
    String j;
    j.reserve(limit > 40 ? limit * 130 : 2048);       // большой — уйдёт в PSRAM
    j = "[";
    bool first = true;
    uint16_t out = 0;
    // Время приходит по NTP через несколько секунд после включения, и события
    // загрузки успевают записаться раньше. Восстанавливаем их время задним
    // числом от millis(): иначе первые записи навсегда остались бы без даты.
    time_t now = time(nullptr);
    bool   haveTime = now > 1700000000;
    // Сначала то, что ещё только в памяти, — оно новее файла; новые сверху.
    for (uint8_t n = 0; n < s_count && out < limit; n++) {
        const Event& e = s_buf[(s_head + CAP - 1 - n) % CAP];
        if (e.saved && s_fs) continue;
        time_t t = e.t;
        if (!t && haveTime) t = now - (time_t)((millis() - e.ms) / 1000);
        put(j, first, t, (millis() - e.ms) / 1000, e.kind, e.detail);
        out++;
    }
    if (s_fs && out < limit) {
        File f = LittleFS.open(F_EV, "r");
        EvHdr h = {0, 0, 0};
        if (f) f.read((uint8_t*)&h, sizeof(h));
        if (h.magic == EV_MAGIC) {
            for (uint16_t n = 0; n < h.count && out < limit; n++) {
                uint16_t slot = (h.next + NFILE - 1 - n) % NFILE;
                EvRec r;
                f.seek(sizeof(EvHdr) + slot * sizeof(EvRec));
                if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
                r.detail[sizeof(r.detail) - 1] = 0;
                uint32_t ago = r.t && haveTime && (time_t)r.t <= now ? (uint32_t)(now - r.t) : 0;
                put(j, first, r.t, ago, (EventKind)r.kind, r.detail);
                out++;
            }
        }
        if (f) f.close();
    }
    return j + "]";
}

// ---------------------------------------------------------------- наблюдатель

// Сравниваем снапшот с прошлым и сами заводим события. Так производители данных
// (задача BLE, сеть) не обязаны знать про журнал — им хватает своего дела.
void evWatchLoop() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    flushToFile();

    static bool first = true;
    static bool pFresh = false, pChg = true, pDsg = true, pFault = false;

    BmsState s = stateSnapshot();

    bool fault = false;
    if (s.haveFaults)
        for (uint8_t i = 0; i < 7; i++) if (s.faults[i]) fault = true;

    if (first) {
        first  = false;
        pFresh = s.fresh; pChg = s.chargeMos; pDsg = s.dischargeMos; pFault = fault;
        return;
    }

    // Ключи и аварии имеют смысл только когда данные живые: у мёртвой связи
    // все поля нулевые, и без этой проверки журнал завалило бы ложными авариями.
    if (s.fresh) {
        if (!pChg && s.chargeMos)      evAdd(EV_CHG_OK);
        if (pChg && !s.chargeMos)      evAdd(EV_CHG_BLOCKED);
        if (!pDsg && s.dischargeMos)   evAdd(EV_DSG_OK);
        if (pDsg && !s.dischargeMos)   evAdd(EV_DSG_BLOCKED);

        if (!pFault && fault) {
            char d[24];
            snprintf(d, sizeof(d), "%02X %02X %02X", s.faults[0], s.faults[1], s.faults[2]);
            evAdd(EV_FAULT_ON, d);
        }
        if (pFault && !fault) evAdd(EV_FAULT_OFF);

        pChg = s.chargeMos; pDsg = s.dischargeMos; pFault = fault;
    }

    if (pFresh && !s.fresh && s.linked) evAdd(EV_BMS_STALE);
    pFresh = s.fresh;

    // ------------------------------------------------------------- инвертор
    static bool    pInvLink = false, pGrid = false, pInvFirst = true;
    static uint8_t pMode = INVM_UNKNOWN, pFaultCode = 0;

    InvState v = invSnapshot();

    if (pInvFirst) {
        pInvFirst  = false;
        pInvLink   = v.linked;
        pGrid      = v.gridOn;
        pMode      = v.mode;
        pFaultCode = v.faultCode;
        return;
    }

    if (!pInvLink && v.linked) evAdd(EV_INV_UP);
    if (pInvLink && !v.linked) evAdd(EV_INV_DOWN);
    pInvLink = v.linked;

    if (!v.fresh) return;      // без живых данных сравнивать нечего

    if (pGrid && !v.gridOn) evAdd(EV_GRID_LOST);
    if (!pGrid && v.gridOn) evAdd(EV_GRID_BACK);
    pGrid = v.gridOn;

    if (pMode != v.mode) {
        evAdd(EV_INV_MODE, invModeName(v.mode));
        pMode = v.mode;
    }

    if (v.faultCode && v.faultCode != pFaultCode) {
        char d[16];
        snprintf(d, sizeof(d), "код %u", v.faultCode);
        evAdd(EV_INV_FAULT, d);
    }
    pFaultCode = v.faultCode;
}

const char* invModeName(uint8_t m) {
    switch (m) {
    case INVM_STANDBY: return "ожидание";
    case INVM_LINE:    return "работа от сети";
    case INVM_BATTERY: return "работа от батареи";
    case INVM_BYPASS:  return "обход";
    case INVM_FAULT:   return "авария";
    }
    return "нет связи";
}
