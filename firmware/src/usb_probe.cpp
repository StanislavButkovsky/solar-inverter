// Разведка USB инвертора. Описание — в usb_probe.h.
//
// Устроено на голом USB-хосте ESP-IDF, без классовых драйверов: заранее
// неизвестно, мост там или HID, а дескрипторы и пара управляющих посылок —
// ровно та часть, что понадобится и настоящему драйверу.
//
// Две задачи. Первая крутит саму библиотеку хоста. Вторая — клиент: ловит
// подключение и ведёт разведку. Все колбэки передач зовутся из
// usb_host_client_handle_events(), то есть в задаче клиента, поэтому
// ожидание передачи — это просто обработка событий в цикле с крайним сроком.
#if defined(TARGET_HS35)

#include "usb_probe.h"
#include "modbus.h"
#include "voltronic.h"
#include <Preferences.h>
#include <memory>
#include <stdarg.h>
#include "usb/usb_host.h"

static Preferences       s_prefs;
static SemaphoreHandle_t s_mtx;
static String            s_report;          // опубликованный отчёт, под мьютексом
static volatile uint8_t  s_state = 0;       // 0 ждёт, 1 разведка, 2 готово
static uint32_t          s_count = 0;

static usb_host_client_handle_t s_client = nullptr;
static usb_device_handle_t      s_dev    = nullptr;
static volatile uint8_t         s_newAddr = 0;
static volatile bool            s_gone    = false;

static String s_txt;                        // отчёт в работе, только задача клиента

// ------------------------------------------------------------------ отчёт

static void add(const char* fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    s_txt += b;
    s_txt += '\n';
    Serial.printf("[usb] %s\n", b);
}

static void addHex(const char* title, const uint8_t* d, size_t n) {
    String h = title;
    size_t shown = n > 64 ? 64 : n;
    for (size_t i = 0; i < shown; i++) {
        char x[4];
        snprintf(x, sizeof(x), " %02X", d[i]);
        h += x;
    }
    if (n > shown) h += " …";
    add("%s", h.c_str());
}

// То же, но печатными символами: у Voltronic ответ — строка.
static void addText(const char* title, const uint8_t* d, size_t n) {
    String t = title;
    for (size_t i = 0; i < n && i < 160; i++)
        t += (d[i] >= 0x20 && d[i] < 0x7F) ? (char)d[i] : '.';
    add("%s", t.c_str());
}

// Публикуем по ходу, а не только в конце: если инвертор выдернут на середине
// или плата зависнет, дескрипторы уже будут сохранены.
// Блобом, а не строкой: строковое значение NVS упирается в 4 КБ, а отчёт
// с перебором команд и русским текстом (два байта на букву) бывает длиннее.
static void publish() {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_report = s_txt;
    xSemaphoreGive(s_mtx);
    if (s_prefs.putBytes("repb", s_txt.c_str(), s_txt.length()) != s_txt.length())
        Serial.printf("[usb] отчёт в NVS НЕ записался (%u байт)\n", (unsigned)s_txt.length());
}

// ------------------------------------------------------------------ передачи

static usb_transfer_t* s_ctrl = nullptr;
static usb_transfer_t* s_out  = nullptr;
static usb_transfer_t* s_in   = nullptr;
static uint8_t         s_mps0 = 8;
static volatile bool   s_ctrlDone, s_outDone;
static volatile bool   s_inRun = false;
static uint8_t         s_rx[512];
static volatile size_t s_rxLen = 0;

static void ctrlCb(usb_transfer_t*) { s_ctrlDone = true; }
static void outCb(usb_transfer_t*)  { s_outDone = true; }

// Приём крутится сам: пришёл кусок — дописали и сразу заказали следующий.
// Пока устройство молчит, передача просто висит, это нормально для bulk.
static void inCb(usb_transfer_t* t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        size_t room = sizeof(s_rx) - s_rxLen;
        size_t n = (size_t)t->actual_num_bytes < room ? t->actual_num_bytes : room;
        memcpy(s_rx + s_rxLen, t->data_buffer, n);
        s_rxLen += n;
    }
    if (s_inRun && t->status == USB_TRANSFER_STATUS_COMPLETED)
        usb_host_transfer_submit(t);
    else
        s_inRun = false;
}

static void pump(uint32_t ms) {
    uint32_t t0 = millis();
    do usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
    while (millis() - t0 < ms);
}

static bool waitFlag(volatile bool& f, uint32_t ms) {
    uint32_t t0 = millis();
    while (!f && millis() - t0 < ms)
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
    return f;
}

static uint16_t roundMps(uint16_t n, uint16_t mps) { return mps ? (n + mps - 1) / mps * mps : n; }

// Управляющая посылка на нулевую точку. Для IN данные копируются в data.
static bool control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
                    uint8_t* data, uint16_t len, uint16_t* got = nullptr) {
    usb_setup_packet_t* sp = (usb_setup_packet_t*)s_ctrl->data_buffer;
    sp->bmRequestType = type;
    sp->bRequest      = req;
    sp->wValue        = val;
    sp->wIndex        = idx;
    sp->wLength       = len;
    bool in = type & 0x80;
    if (!in && len) memcpy(s_ctrl->data_buffer + 8, data, len);
    s_ctrl->num_bytes = 8 + (in ? roundMps(len, s_mps0) : len);
    s_ctrl->device_handle    = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback         = ctrlCb;
    s_ctrlDone = false;
    if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) return false;
    if (!waitFlag(s_ctrlDone, 1500)) return false;
    if (s_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return false;
    if (in && data) {
        int n = s_ctrl->actual_num_bytes - 8;
        if (n < 0) n = 0;
        if (n > len) n = len;
        memcpy(data, s_ctrl->data_buffer + 8, n);
        if (got) *got = n;
    }
    return true;
}

static bool sendOut(uint8_t ep, const uint8_t* d, size_t n) {
    memcpy(s_out->data_buffer, d, n);
    s_out->num_bytes        = n;
    s_out->device_handle    = s_dev;
    s_out->bEndpointAddress = ep;
    s_out->callback         = outCb;
    s_outDone = false;
    if (usb_host_transfer_submit(s_out) != ESP_OK) return false;
    return waitFlag(s_outDone, 1000) && s_out->status == USB_TRANSFER_STATUS_COMPLETED;
}

static bool startIn(uint8_t ep, uint16_t mps) {
    s_in->num_bytes        = mps;
    s_in->device_handle    = s_dev;
    s_in->bEndpointAddress = ep;
    s_in->callback         = inCb;
    s_inRun = true;
    if (usb_host_transfer_submit(s_in) != ESP_OK) { s_inRun = false; return false; }
    return true;
}

// Висящую передачу приёма снимаем через остановку точки: иначе она
// так и осталась бы в очереди, и закрыть устройство было бы нельзя.
static void stopIn(uint8_t ep) {
    if (!s_inRun) return;
    s_inRun = false;
    usb_host_endpoint_halt(s_dev, ep);
    usb_host_endpoint_flush(s_dev, ep);
    pump(50);
    usb_host_endpoint_clear(s_dev, ep);
}

// Ждём ответ: Modbus узнаём по длине из третьего байта, Voltronic — по \r.
// Дальше паузы в 300 мс считаем концом ответа.
static size_t collect(uint32_t ms) {
    uint32_t t0 = millis(), lastGrow = 0;
    size_t seen = 0;
    while (millis() - t0 < ms) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
        if (s_rxLen != seen) { seen = s_rxLen; lastGrow = millis(); }
        if (seen && millis() - lastGrow > 300) break;
        if (s_gone) break;
    }
    return s_rxLen;
}

// ------------------------------------------------------------------ мосты

enum Kind { K_NONE, K_CH34X, K_CP210X, K_CDC, K_HID };

static const char* knownName(uint16_t vid, uint16_t pid) {
    if (vid == 0x1A86 && pid == 0x7523) return "CH340 — мост в последовательный порт";
    if (vid == 0x1A86 && pid == 0x5523) return "CH341 — мост в последовательный порт";
    if (vid == 0x1A86 && (pid == 0x55D3 || pid == 0x55D4)) return "CH343/CH9102 — мост, класс CDC";
    if (vid == 0x10C4 && pid == 0xEA60) return "CP210x — мост в последовательный порт";
    if (vid == 0x0403) return "FTDI — мост в последовательный порт";
    if (vid == 0x067B) return "PL2303 — мост в последовательный порт";
    if (vid == 0x0665 && pid == 0x5161) return "HID Voltronic (команды отчётами по 8 байт)";
    return "неизвестно";
}

struct Link {
    Kind    kind = K_NONE;
    uint8_t ifc = 0, ifcComm = 0;     // интерфейс данных и (для CDC) управления
    uint8_t epIn = 0, epOut = 0;
    uint16_t mpsIn = 64;
};

// Скорость и формат 8N1. Значения CH34x — по драйверу ch341 из ядра Linux.
static bool setLine(const Link& L, uint32_t baud, uint16_t chVer) {
    switch (L.kind) {
    case K_CH34X: {
        // Делитель от 48 МГц: (0x100 - div) << 8 | fact << 2 | ps.
        uint16_t v;
        switch (baud) {
        case 2400:  v = 0xD901; break;
        case 9600:  v = 0xB202; break;
        case 19200: v = 0xD902; break;
        default:    return false;
        }
        if (chVer > 0x27) v |= 0x80;        // у новых ревизий — без буферизации
        if (!control(0x40, 0x9A, 0x1312, v, nullptr, 0)) return false;
        control(0x40, 0x9A, 0x2518, 0x00C3, nullptr, 0);   // 8N1, приём и передача
        return true;
    }
    case K_CP210X: {
        uint8_t b[4] = { (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), 0 };
        if (!control(0x41, 0x1E, 0, L.ifc, b, 4)) return false;   // SET_BAUDRATE
        control(0x41, 0x03, 0x0800, L.ifc, nullptr, 0);         // 8N1
        return true;
    }
    case K_CDC: {
        uint8_t lc[7] = { (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16),
                          (uint8_t)(baud >> 24), 0, 0, 8 };
        return control(0x21, 0x20, 0, L.ifcComm, lc, 7);       // SET_LINE_CODING
    }
    default:
        return false;
    }
}

// Однократная подготовка моста: включить, поднять DTR/RTS.
static uint16_t openBridge(const Link& L) {
    uint16_t ver = 0;
    switch (L.kind) {
    case K_CH34X: {
        uint8_t v[2] = {0, 0};
        uint16_t got = 0;
        if (control(0xC0, 0x5F, 0, 0, v, 2, &got) && got >= 1) ver = v[0];
        add("CH34x: версия чипа 0x%02X", ver);
        control(0x40, 0xA1, 0, 0, nullptr, 0);             // инициализация порта
        control(0x40, 0xA4, (uint16_t)~0x60, 0, nullptr, 0); // DTR и RTS
        break;
    }
    case K_CP210X:
        control(0x41, 0x00, 1, L.ifc, nullptr, 0);         // IFC_ENABLE
        control(0x41, 0x07, 0x0303, L.ifc, nullptr, 0);    // DTR и RTS
        break;
    case K_CDC:
        control(0x21, 0x22, 0x0003, L.ifcComm, nullptr, 0); // DTR и RTS
        break;
    default: break;
    }
    return ver;
}

// ------------------------------------------------------------------ попытки

static const uint16_t MB_START = 25201, MB_COUNT = 16;

// Разобрать ответ Modbus. true — протокол угадан (в том числе ответом-ошибкой).
static bool explainModbus(uint8_t slave, const uint8_t* r, size_t n) {
    if (n >= 5 && r[0] == slave && (r[1] & 0x80)) {
        add("  ответ-ошибка Modbus, код %u: протокол и адрес верные, регистра нет", r[2]);
        return true;
    }
    if (n < 5 || r[0] != slave || r[1] != 0x03) {
        add("  на Modbus не похоже");
        return false;
    }
    size_t need = 3 + r[2] + 2;
    if (n < need) { add("  кадр Modbus обрезан: %u из %u байт", (unsigned)n, (unsigned)need); return true; }
    uint16_t crc = r[need - 2] | (r[need - 1] << 8);
    if (crc != mbCrc16(r, need - 2)) { add("  кадр Modbus, но контрольная сумма не сошлась"); return true; }

    add("  Modbus RTU отвечает, контрольная сумма верна");
    auto R = [&](uint16_t reg) -> int {
        int i = reg - MB_START;
        return (i >= 0 && i * 2 + 1 < r[2]) ? (int)((r[3 + i * 2] << 8) | r[4 + i * 2]) : -1;
    };
    add("  25201 режим работы       %d", R(25201));
    add("  25205 батарея            %.1f В", R(25205) * 0.1f);
    add("  25206 выход              %.1f В", R(25206) * 0.1f);
    add("  25207 сеть               %.1f В", R(25207) * 0.1f);
    add("  25213 мощность инвертора %d Вт", R(25213));
    add("  25216 загрузка           %d %%", R(25216));
    return true;
}

static bool tryModbus(const Link& L, uint32_t baud, uint8_t slave, uint16_t chVer) {
    add("-- %lu 8N1, Modbus адрес %u, регистры %u…%u", (unsigned long)baud, slave,
        MB_START, MB_START + MB_COUNT - 1);
    if (!setLine(L, baud, chVer)) { add("  не удалось выставить скорость"); return false; }
    uint8_t req[8];
    mbBuildRead(req, slave, MB_START, MB_COUNT);
    s_rxLen = 0;
    if (!sendOut(L.epOut, req, sizeof(req))) { add("  отправить не удалось"); return false; }
    size_t n = collect(1500);
    if (!n) { add("  тишина"); return false; }
    uint8_t copy[sizeof(s_rx)];
    memcpy(copy, s_rx, n);
    addHex("  ответ:", copy, n);
    return explainModbus(slave, copy, n);
}

// QPI с готовой суммой XMODEM: "QPI" BE AC \r — самая безобидная команда,
// спрашивает только версию протокола.
static const uint8_t QPI[] = { 'Q', 'P', 'I', 0xBE, 0xAC, 0x0D };

// Текстовые команды-вопросы, все только на чтение. Первая разведка у HS35
// получила на QPI ответ NAK: провод и скорость верные, но этой команды
// инвертор не знает. Поэтому перебираем, на что он ответит по существу:
// классический Voltronic (PI30), затем диалекты с ^P — PI18 и PI17.
static const char* const TEXT_CMDS[] = {
    "QPI", "QID", "QVFW", "QMOD", "QPIGS", "QPIRI", "QPIWS", "QFLAG", "QGMN", "QMN",
    "^P005PI", "^P005ID", "^P005GS", "^P006MOD", "^P003PI",
};

// Ответ одной строкой: печатное как есть, остальное — \xHH, \r — как \r.
static String replyLine(const uint8_t* d, size_t n) {
    String s;
    for (size_t i = 0; i < n && s.length() < 120; i++) {
        if (d[i] == 0x0D) s += "\\r";
        else if (d[i] >= 0x20 && d[i] < 0x7F) s += (char)d[i];
        else { char x[6]; snprintf(x, sizeof(x), "\\x%02X", d[i]); s += x; }
    }
    if (n > 0 && s.length() >= 120) s += "…";
    return s;
}

// 0 — тишина, 1 — NAK (команду не знает), 2 — ответ по существу.
static int askText(const Link& L, const char* cmd, bool crc) {
    uint8_t req[24];
    size_t n;
    if (crc) n = vtBuildCommand(cmd, req, sizeof(req));
    else { n = strlen(cmd); memcpy(req, cmd, n); req[n++] = 0x0D; }
    const char* how = crc ? "" : " без суммы";
    s_rxLen = 0;
    if (!n || !sendOut(L.epOut, req, n)) { add("  %s%s: отправить не удалось", cmd, how); return 0; }
    size_t got = collect(1200);
    if (!got) { add("  %s%s: тишина", cmd, how); return 0; }
    uint8_t copy[sizeof(s_rx)];
    memcpy(copy, s_rx, got);
    add("  %s%s → %s", cmd, how, replyLine(copy, got).c_str());
    bool nak = false;
    for (size_t i = 0; i + 3 <= got; i++)
        if (!memcmp(copy + i, "NAK", 3)) { nak = true; break; }
    return nak ? 1 : 2;
}

// Сначала одна QPI — понять, жив ли текстовый канал на этой скорости.
// Ответил хоть чем-то — перебираем весь набор.
static int tryTextSerial(const Link& L, uint32_t baud, uint16_t chVer, bool sweep) {
    add("-- %lu 8N1, текстовые команды", (unsigned long)baud);
    if (!setLine(L, baud, chVer)) { add("  не удалось выставить скорость"); return 0; }
    if (!sweep) return askText(L, "QPI", true);
    int best = 0;
    for (const char* cmd : TEXT_CMDS) {
        if (s_gone) break;
        int r = askText(L, cmd, true);
        if (r == 1 && !s_gone) {
            int r2 = askText(L, cmd, false);
            if (r2 > r) r = r2;
        }
        if (r > best) best = r;
        publish();
    }
    return best;
}

// HID: команда уходит отчётами SET_REPORT по 8 байт, ответ — с точки прерываний.
static bool hidAsk(const Link& L, const char* name, const uint8_t* cmd, size_t n) {
    add("-- HID, команда %s", name);
    s_rxLen = 0;
    for (size_t off = 0; off < n; off += 8) {
        uint8_t chunk[8] = {0};
        memcpy(chunk, cmd + off, (n - off) < 8 ? (n - off) : 8);
        if (!control(0x21, 0x09, 0x0200, L.ifc, chunk, 8)) { add("  SET_REPORT отклонён"); return false; }
    }
    // Ответ Voltronic длинный и идёт отчётами по 8 байт, ждём \r.
    uint32_t t0 = millis();
    while (millis() - t0 < 3000 && !s_gone) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
        if (s_rxLen && memchr(s_rx, '\r', s_rxLen)) break;
    }
    size_t got = s_rxLen;
    if (!got) { add("  тишина"); return false; }
    uint8_t copy[sizeof(s_rx)];
    memcpy(copy, s_rx, got);
    addHex("  ответ:", copy, got);
    addText("  текстом:", copy, got);
    return copy[0] == '(';
}

// QPIGS с суммой XMODEM — все текущие показания одной строкой.
static const uint8_t QPIGS[] = { 'Q', 'P', 'I', 'G', 'S', 0xB7, 0xA9, 0x0D };

// ------------------------------------------------------------------ разведка

static void probeDevice(uint8_t addr) {
    s_state = 1;
    s_count++;
    s_prefs.putUInt("n", s_count);
    s_txt = "";
    add("=== разведка USB №%lu ===", (unsigned long)s_count);
    add("устройство появилось через %lu с после включения платы", (unsigned long)(millis() / 1000));

    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        add("открыть устройство не удалось");
        publish(); s_state = 2; return;
    }

    usb_device_info_t info;
    usb_host_device_info(s_dev, &info);
    s_mps0 = info.bMaxPacketSize0;
    const usb_device_desc_t* dd;
    usb_host_get_device_descriptor(s_dev, &dd);

    add("VID:PID %04x:%04x — %s", dd->idVendor, dd->idProduct, knownName(dd->idVendor, dd->idProduct));
    add("USB %x.%02x, скорость %s, класс устройства %02X/%02X/%02X, EP0 %u байт",
        dd->bcdUSB >> 8, dd->bcdUSB & 0xFF,
        info.speed == USB_SPEED_LOW ? "low" : info.speed == USB_SPEED_FULL ? "full" : "high",
        dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol, s_mps0);

    auto str = [](const usb_str_desc_t* s) -> String {
        String o;
        if (!s) return String("—");
        for (int i = 0; i < (s->bLength - 2) / 2; i++) {
            uint16_t c = s->wData[i];
            o += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        return o;
    };
    add("производитель: %s", str(info.str_desc_manufacturer).c_str());
    add("изделие:       %s", str(info.str_desc_product).c_str());
    add("серийный №:    %s", str(info.str_desc_serial_num).c_str());

    // Обход конфигурации: интерфейсы и их точки. Заодно выбираем канал.
    Link L;
    const usb_config_desc_t* cd;
    usb_host_get_active_config_descriptor(s_dev, &cd);
    add("конфигурация %u, интерфейсов %u, питание %u мА",
        cd->bConfigurationValue, cd->bNumInterfaces, cd->bMaxPower * 2);

    int off = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cd;
    uint8_t curIfc = 0, curClass = 0;
    int hidIfc = -1, cdcComm = -1, cdcData = -1;
    uint8_t hidIn = 0; uint16_t hidMps = 8;
    uint8_t bIn = 0, bOut = 0, bIfc = 0; uint16_t bMps = 64;
    while ((d = usb_parse_next_descriptor(d, cd->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* it = (const usb_intf_desc_t*)d;
            curIfc = it->bInterfaceNumber;
            curClass = it->bInterfaceClass;
            add("интерфейс %u.%u: класс %02X/%02X/%02X, точек %u", curIfc, it->bAlternateSetting,
                it->bInterfaceClass, it->bInterfaceSubClass, it->bInterfaceProtocol, it->bNumEndpoints);
            if (curClass == 0x03 && hidIfc < 0) hidIfc = curIfc;
            if (curClass == 0x02 && cdcComm < 0) cdcComm = curIfc;
            if (curClass == 0x0A && cdcData < 0) cdcData = curIfc;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
            static const char* T[] = { "control", "iso", "bulk", "interrupt" };
            bool in = ep->bEndpointAddress & 0x80;
            uint8_t type = ep->bmAttributes & 3;
            add("  точка 0x%02X %s %s, пакет %u", ep->bEndpointAddress, in ? "IN " : "OUT",
                T[type], ep->wMaxPacketSize);
            if (type == 2 && !bIfc && (!bIn || !bOut)) {
                if (in  && !bIn)  { bIn = ep->bEndpointAddress; bMps = ep->wMaxPacketSize; }
                if (!in && !bOut) bOut = ep->bEndpointAddress;
                if (bIn && bOut) bIfc = curIfc + 1;      // +1: ноль — тоже номер
            }
            if (type == 3 && in && curClass == 0x03 && !hidIn) { hidIn = ep->bEndpointAddress; hidMps = ep->wMaxPacketSize; }
        }
    }
    publish();

    uint16_t vid = dd->idVendor;
    if (cdcData >= 0 && bIfc)               L.kind = K_CDC;
    else if (vid == 0x1A86 && bIfc)         L.kind = K_CH34X;
    else if (vid == 0x10C4 && bIfc)         L.kind = K_CP210X;
    else if (hidIfc >= 0 && hidIn)          L.kind = K_HID;

    if (L.kind == K_HID) { L.ifc = hidIfc; L.epIn = hidIn; L.mpsIn = hidMps; }
    else if (L.kind != K_NONE) {
        L.ifc = bIfc - 1; L.epIn = bIn; L.epOut = bOut; L.mpsIn = bMps;
        L.ifcComm = cdcComm >= 0 ? cdcComm : L.ifc;
    }

    static const char* KN[] = { "не опознан", "мост CH34x", "мост CP210x", "CDC-ACM", "HID" };
    add("как говорить: %s", KN[L.kind]);

    if (L.kind == K_NONE) {
        add("итог: способ связи не опознан — пришлите этот отчёт, разберёмся по дескрипторам");
    } else {
        bool claimed = usb_host_interface_claim(s_client, s_dev, L.ifc, 0) == ESP_OK;
        if (L.kind == K_CDC && L.ifcComm != L.ifc)
            usb_host_interface_claim(s_client, s_dev, L.ifcComm, 0);
        if (!claimed) add("занять интерфейс %u не удалось", L.ifc);

        usb_host_transfer_alloc(64 + 8, 0, &s_ctrl);
        usb_host_transfer_alloc(64, 0, &s_out);
        usb_host_transfer_alloc(L.mpsIn > 64 ? L.mpsIn : 64, 0, &s_in);

        bool ok = false;
        if (claimed && startIn(L.epIn, L.mpsIn)) {
            if (L.kind == K_HID) {
                ok = hidAsk(L, "QPI", QPI, sizeof(QPI));
                if (ok) hidAsk(L, "QPIGS", QPIGS, sizeof(QPIGS));
                publish();
            } else {
                uint16_t ver = openBridge(L);
                // Сначала то, что ждём от HS35, потом соседние варианты.
                struct { uint32_t baud; uint8_t slave; } mb[] = { {19200, 4}, {9600, 4}, {19200, 1}, {9600, 1} };
                for (auto& t : mb) {
                    if (s_gone) break;
                    ok = tryModbus(L, t.baud, t.slave, ver);
                    publish();
                    if (ok) break;
                }
                // Не Modbus — значит текстовый протокол. Скорость ищем одной
                // QPI, а на той, где хоть что-то ответило, перебираем набор.
                // У HS35 так отозвалась 9600, поэтому она первая.
                static const uint32_t textBaud[] = { 9600, 2400 };
                for (uint32_t b : textBaud) {
                    if (ok || s_gone) break;
                    int r = tryTextSerial(L, b, ver, false);
                    publish();
                    if (r == 0) continue;
                    r = tryTextSerial(L, b, ver, true);
                    ok = r == 2;
                    if (!ok) add("  на %lu отвечает только NAK: протокол не опознан", (unsigned long)b);
                    publish();
                    break;
                }
            }
            stopIn(L.epIn);
        } else if (claimed) {
            add("запустить приём не удалось");
        }

        if (s_gone) add("устройство отключили во время разведки");
        add(ok ? "итог: связь есть, протокол опознан" : "итог: устройство видно, но на запросы не ответило");

        usb_host_transfer_free(s_ctrl); s_ctrl = nullptr;
        usb_host_transfer_free(s_out);  s_out  = nullptr;
        usb_host_transfer_free(s_in);   s_in   = nullptr;
        usb_host_interface_release(s_client, s_dev, L.ifc);
        if (L.kind == K_CDC && L.ifcComm != L.ifc)
            usb_host_interface_release(s_client, s_dev, L.ifcComm);
    }

    usb_host_device_close(s_client, s_dev);
    s_dev = nullptr;
    publish();
    s_state = 2;
}

// ------------------------------------------------------------------ задачи

static void clientCb(const usb_host_client_event_msg_t* m, void*) {
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV)       s_newAddr = m->new_dev.address;
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) s_gone = true;
}

// Самый ранний след устройства: библиотека зовёт это, как только прочитала
// дескриптор, ещё до выбора конфигурации. Если дальше подключение сорвётся,
// событие NEW_DEV не придёт вовсе — а VID:PID у нас уже будет.
static volatile bool     s_enumSeen = false;
static volatile uint32_t s_enumMs   = 0;
static usb_device_desc_t s_enumDesc;

static bool enumFilter(const usb_device_desc_t* d, uint8_t* bConfigurationValue) {
    memcpy(&s_enumDesc, d, sizeof(s_enumDesc));
    s_enumMs   = millis();
    s_enumSeen = true;
    *bConfigurationValue = 1;
    return true;
}

static void reportStuckEnum() {
    s_count++;
    s_prefs.putUInt("n", s_count);
    s_txt = "";
    add("=== разведка USB №%lu ===", (unsigned long)s_count);
    add("устройство ответило дескриптором через %lu с после включения платы",
        (unsigned long)(s_enumMs / 1000));
    add("VID:PID %04x:%04x — %s", s_enumDesc.idVendor, s_enumDesc.idProduct,
        knownName(s_enumDesc.idVendor, s_enumDesc.idProduct));
    add("класс устройства %02X/%02X/%02X, EP0 %u байт", s_enumDesc.bDeviceClass,
        s_enumDesc.bDeviceSubClass, s_enumDesc.bDeviceProtocol, s_enumDesc.bMaxPacketSize0);
    add("итог: подключение не завершилось — выбор конфигурации сорвался");
    publish();
    s_state = 2;
}

static void hostTask(void*) {
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void clientTask(void*) {
    usb_host_client_config_t cc = {};
    cc.is_synchronous    = false;
    cc.max_num_event_msg = 5;
    cc.async.client_event_callback = clientCb;
    cc.async.callback_arg = nullptr;
    if (usb_host_client_register(&cc, &s_client) != ESP_OK) {
        Serial.println("[usb] клиент USB-хоста не зарегистрировался");
        vTaskDelete(nullptr);
    }
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        if (s_newAddr) {
            uint8_t a = s_newAddr;
            s_newAddr = 0;
            s_gone = false;
            s_enumSeen = false;
            probeDevice(a);
        }
        // Сколько устройств видит сама библиотека, независимо от событий:
        // если здесь больше нуля, а разведки не было — ошибка у нас, а не в проводе.
        static uint32_t lastDiag = 0;
        static int      lastDevs = -1;
        if (millis() - lastDiag > 2000) {
            lastDiag = millis();
            usb_host_lib_info_t li;
            if (usb_host_lib_info(&li) == ESP_OK && li.num_devices != lastDevs) {
                lastDevs = li.num_devices;
                Serial.printf("[usb] устройств на шине: %d, клиентов: %d\n", li.num_devices, li.num_clients);
            }
        }
        // Дескриптор был, а события о новом устройстве за 5 с так и нет.
        if (s_enumSeen && millis() - s_enumMs > 5000) {
            s_enumSeen = false;
            reportStuckEnum();
        }
    }
}

void usbProbeStart() {
    s_mtx = xSemaphoreCreateMutex();
    // Отчёт ценен ровно тем, что переживает питание: первый из них, снятый
    // у инвертора, дома в памяти не нашёлся. Поэтому здесь проверка на
    // каждой загрузке — пространство открылось и счётчик загрузок растёт.
    bool opened = s_prefs.begin("usbprobe", false);
    uint32_t boots = s_prefs.getUInt("boots", 0) + 1;
    size_t wrote = s_prefs.putUInt("boots", boots);
    Serial.printf("[usb] NVS разведки: %s, загрузка №%lu%s\n", opened ? "открыто" : "НЕ ОТКРЫЛОСЬ",
                  (unsigned long)boots, wrote ? "" : ", запись НЕ ПРОШЛА");
    size_t len = s_prefs.getBytesLength("repb");
    if (len) {
        std::unique_ptr<char[]> b(new char[len + 1]);
        s_prefs.getBytes("repb", b.get(), len);
        b[len] = 0;
        s_report = b.get();
    } else {
        s_report = s_prefs.getString("rep", "");   // отчёт прошлой версии, строкой
    }
    s_count  = s_prefs.getUInt("n", 0);
    Serial.printf("[usb] сохранённый отчёт разведки: %s (команда usb — показать)\n",
                  s_report.length() ? "есть" : "нет");

    // Ошибки самой библиотеки хоста — в терминал: если подключение срывается
    // до дескриптора, кроме них о нём не узнать ничего. Выше ERROR библиотеки
    // ядра всё равно не собраны.
    static const char* tags[] = { "USBH", "ENUM", "HUB", "EXT_HUB", "EXT_PORT", "HCD DWC", "USB HOST" };
    for (const char* t : tags) esp_log_level_set(t, ESP_LOG_ERROR);

    usb_host_config_t hc = {};
    hc.skip_phy_setup = false;
    hc.intr_flags     = ESP_INTR_FLAG_LEVEL1;
    hc.enum_filter_cb = enumFilter;
    if (usb_host_install(&hc) != ESP_OK) {
        Serial.println("[usb] USB-хост не поднялся");
        return;
    }
    xTaskCreatePinnedToCore(hostTask,   "usbh",   3072, nullptr, 6, nullptr, 1);
    xTaskCreatePinnedToCore(clientTask, "usbprb", 8192, nullptr, 5, nullptr, 1);
    Serial.println("[usb] хост поднят, жду устройство на разъёме USB");
}

String usbProbeReport() {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    String r = s_report;
    xSemaphoreGive(s_mtx);
    return r;
}

const char* usbProbeState() {
    return s_state == 1 ? "busy" : s_state == 2 ? "done" : "wait";
}

uint32_t usbProbeCount() { return s_count; }

#endif
