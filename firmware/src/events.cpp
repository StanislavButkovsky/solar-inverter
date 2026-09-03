#include "events.h"
#include "state.h"
#include <time.h>

static const uint8_t CAP = 40;

struct Event {
    uint32_t   ms;
    time_t     t;         // 0 — время ещё не синхронизировано
    EventKind  kind;
    char       detail[40];   // хватает на «работа от батареи» (33 байта в UTF-8)
};

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

String evJson() {
    String j = "[";
    // Время приходит по NTP через несколько секунд после включения, и события
    // загрузки успевают записаться раньше. Восстанавливаем их время задним
    // числом от millis(): иначе первые записи навсегда остались бы без даты.
    time_t now = time(nullptr);
    bool   haveTime = now > 1700000000;
    // новые сверху: идём от последней записи назад
    for (uint8_t n = 0; n < s_count; n++) {
        uint8_t i = (s_head + CAP - 1 - n) % CAP;
        const Event& e = s_buf[i];
        if (n) j += ",";

        char when[24] = {0};
        time_t t = e.t;
        if (!t && haveTime) t = now - (time_t)((millis() - e.ms) / 1000);
        if (t) {
            struct tm tmv;
            localtime_r(&t, &tmv);
            snprintf(when, sizeof(when), "%02d.%02d %02d:%02d:%02d",
                     tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }

        j += "{\"when\":\"" + String(when) + "\"";
        j += ",\"ago\":" + String((millis() - e.ms) / 1000);
        j += ",\"level\":\"" + String(levelName(evLevel(e.kind))) + "\"";
        j += ",\"text\":\"" + String(evText(e.kind)) + "\"";
        j += ",\"detail\":\"" + String(e.detail) + "\"}";
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
