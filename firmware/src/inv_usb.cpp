// Транспорт до инвертора по USB: встроенный мост CH34x (VID 0x1A86).
//
// Плата в роли хоста, инвертор — устройство. Обязателен ESP32-S3: у обычного
// ESP32 контроллера USB-host нет ни аппаратно, ни программно. Питание шины
// даёт плата: на YD-ESP32-S3 для этого замыкается перемычка USB-OTG.
//
// Устроен на голом USB-хосте ESP-IDF, тем же путём, что разведка
// (usb_probe.cpp), которой он и проверен на живом HS35 19.10.2026.
//
// Две задачи: библиотека хоста и клиент. Клиент делает всю работу с USB —
// открытие, настройку моста, приём и отправку. invTrSend() из задачи опроса
// только кладёт кадр в очередь: отправка побайтная, с паузой INV_GAP_MS,
// и держать на ней цикл BLE незачем.
#if defined(TARGET_HS35)

#include "inv.h"
#include "inv_transport.h"
#include "profile.h"

#if defined(USB_PROBE)

// Сборка разведки: USB целиком у usb_probe.cpp, опроса нет.
bool invLooksLikeDongle(const String&, bool) { return false; }
void invTrBegin()     { Serial.println("[инв] сборка разведки: USB занят разведкой, опроса нет"); }
bool invTrConnected() { return false; }
bool invTrConnect()   { return false; }
void invTrSend(const uint8_t*, size_t) { }
void invBind(const String&) { }

#else

#include "usb/usb_host.h"

static usb_host_client_handle_t s_client = nullptr;
static usb_device_handle_t      s_dev    = nullptr;
static volatile uint8_t         s_newAddr = 0;
static volatile bool            s_gone    = false;
static volatile bool            s_ready   = false;

static usb_transfer_t* s_ctrl = nullptr;
static usb_transfer_t* s_out  = nullptr;
static usb_transfer_t* s_in   = nullptr;
static uint8_t  s_ifc = 0, s_epIn = 0, s_epOut = 0, s_mps0 = 8;
static uint16_t s_mpsIn = 32;
static volatile bool s_ctrlDone, s_outDone, s_inRun = false;

// Очередь отправки: один кадр, его кладёт invTrSend(), забирает клиент.
static portMUX_TYPE     s_txMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t          s_tx[64];
static volatile size_t  s_txLen = 0;

// ------------------------------------------------------------------ передачи

static void ctrlCb(usb_transfer_t*) { s_ctrlDone = true; }
static void outCb(usb_transfer_t*)  { s_outDone = true; }

// Приём крутится сам: кусок пришёл — отдали профилю, заказали следующий.
static void inCb(usb_transfer_t* t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0)
        invOnBytes(t->data_buffer, t->actual_num_bytes);
    if (s_inRun && t->status == USB_TRANSFER_STATUS_COMPLETED) usb_host_transfer_submit(t);
    else s_inRun = false;
}

static bool waitFlag(volatile bool& f, uint32_t ms) {
    uint32_t t0 = millis();
    while (!f && millis() - t0 < ms)
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
    return f;
}

static void pump(uint32_t ms) {
    uint32_t t0 = millis();
    do usb_host_client_handle_events(s_client, pdMS_TO_TICKS(2));
    while (millis() - t0 < ms);
}

static bool control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len) {
    usb_setup_packet_t* sp = (usb_setup_packet_t*)s_ctrl->data_buffer;
    sp->bmRequestType = type;
    sp->bRequest = req;
    sp->wValue = val;
    sp->wIndex = idx;
    sp->wLength = len;
    bool in = type & 0x80;
    uint16_t room = in ? (uint16_t)((len + s_mps0 - 1) / s_mps0 * s_mps0) : len;
    s_ctrl->num_bytes = 8 + room;
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = ctrlCb;
    s_ctrlDone = false;
    if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) return false;
    if (!waitFlag(s_ctrlDone, 1500) || s_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return false;
    if (in && data) {
        int n = s_ctrl->actual_num_bytes - 8;
        memcpy(data, s_ctrl->data_buffer + 8, n < 0 ? 0 : (n > len ? len : n));
    }
    return true;
}

static bool sendOut(const uint8_t* d, size_t n) {
    memcpy(s_out->data_buffer, d, n);
    s_out->num_bytes = n;
    s_out->device_handle = s_dev;
    s_out->bEndpointAddress = s_epOut;
    s_out->callback = outCb;
    s_outDone = false;
    if (usb_host_transfer_submit(s_out) != ESP_OK) return false;
    return waitFlag(s_outDone, 1000) && s_out->status == USB_TRANSFER_STATUS_COMPLETED;
}

// ------------------------------------------------------------------ мост CH34x

// Делитель от 48 МГц, как ch341_get_divisor() в Linux. Для 9600 — 0xB202.
static int ch341Divisor(uint32_t speed) {
    const int64_t CLK = 48000000;
    auto clkDiv = [](int ps, int fact) -> int64_t { return 1LL << (12 - 3 * ps - fact); };
    int ps, fact = 1;
    for (ps = 3; ps >= 0; ps--)
        if (speed > CLK / (clkDiv(ps, 1) * 512)) break;
    if (ps < 0) return -1;
    int64_t cd = clkDiv(ps, fact), div = CLK / (cd * speed);
    if (div < 9 || div > 255) { div /= 2; cd *= 2; fact = 0; }
    if (div < 2) return -1;
    if (16 * CLK / (cd * div) - 16 * (int64_t)speed >= 16 * (int64_t)speed - 16 * CLK / (cd * (div + 1)))
        div++;
    if (fact == 1 && div % 2 == 0) { div /= 2; fact = 0; }
    return (int)((0x100 - div) << 8 | fact << 2 | ps);
}

static bool bridgeInit() {
    uint8_t ver[2] = {0, 0};
    control(0xC0, 0x5F, 0, 0, ver, 2);                  // версия чипа
    control(0x40, 0xA1, 0, 0, nullptr, 0);              // инициализация порта
    int d = ch341Divisor(INV_BAUD);
    if (d < 0) return false;
    uint16_t v = (uint16_t)d;
    if (ver[0] > 0x27) v |= 0x80;                       // у новых ревизий — без буферизации
    if (!control(0x40, 0x9A, 0x1312, v, nullptr, 0)) return false;
    control(0x40, 0x9A, 0x2518, 0x00C3, nullptr, 0);    // 8N1, приём и передача
    // DTR и RTS опущены — так открывает порт ESPHome; на HS35 разведка
    // показала, что положение линий на ответ не влияет.
    control(0x40, 0xA4, 0xFFFF, 0, nullptr, 0);
    Serial.printf("[инв] мост CH34x 0x%02X, %lu 8N1\n", ver[0], (unsigned long)INV_BAUD);
    return true;
}

// ------------------------------------------------------------------ устройство

static void closeDevice() {
    s_ready = false;
    if (!s_dev) return;
    if (s_inRun) {
        s_inRun = false;
        usb_host_endpoint_halt(s_dev, s_epIn);
        usb_host_endpoint_flush(s_dev, s_epIn);
        pump(50);
        usb_host_endpoint_clear(s_dev, s_epIn);
    }
    if (s_ctrl) { usb_host_transfer_free(s_ctrl); s_ctrl = nullptr; }
    if (s_out)  { usb_host_transfer_free(s_out);  s_out  = nullptr; }
    if (s_in)   { usb_host_transfer_free(s_in);   s_in   = nullptr; }
    usb_host_interface_release(s_client, s_dev, s_ifc);
    usb_host_device_close(s_client, s_dev);
    s_dev = nullptr;
}

static void openDevice(uint8_t addr) {
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) { s_dev = nullptr; return; }
    const usb_device_desc_t* dd;
    usb_host_get_device_descriptor(s_dev, &dd);
    usb_device_info_t info;
    usb_host_device_info(s_dev, &info);
    s_mps0 = info.bMaxPacketSize0;

    if (dd->idVendor != 0x1A86) {
        Serial.printf("[инв] на USB %04x:%04x — не мост CH34x, не трогаю\n", dd->idVendor, dd->idProduct);
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
        return;
    }

    // Первый интерфейс с парой bulk-точек — канал данных.
    const usb_config_desc_t* cd;
    usb_host_get_active_config_descriptor(s_dev, &cd);
    int off = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cd;
    uint8_t cur = 0;
    s_epIn = s_epOut = 0;
    while ((d = usb_parse_next_descriptor(d, cd->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            cur = ((const usb_intf_desc_t*)d)->bInterfaceNumber;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
            if ((ep->bmAttributes & 3) != 2) continue;          // только bulk
            if (ep->bEndpointAddress & 0x80) { if (!s_epIn) { s_epIn = ep->bEndpointAddress; s_mpsIn = ep->wMaxPacketSize; s_ifc = cur; } }
            else if (!s_epOut) s_epOut = ep->bEndpointAddress;
        }
    }
    if (!s_epIn || !s_epOut || usb_host_interface_claim(s_client, s_dev, s_ifc, 0) != ESP_OK) {
        Serial.println("[инв] канал данных моста не найден");
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
        return;
    }

    usb_host_transfer_alloc(64 + 8, 0, &s_ctrl);
    usb_host_transfer_alloc(64, 0, &s_out);
    usb_host_transfer_alloc(s_mpsIn > 64 ? s_mpsIn : 64, 0, &s_in);

    if (!bridgeInit()) { Serial.println("[инв] мост не настроился"); closeDevice(); return; }

    s_in->num_bytes = s_mpsIn;
    s_in->device_handle = s_dev;
    s_in->bEndpointAddress = s_epIn;
    s_in->callback = inCb;
    s_inRun = true;
    if (usb_host_transfer_submit(s_in) != ESP_OK) { s_inRun = false; closeDevice(); return; }

    s_ready = true;
    Serial.println("[инв] инвертор на USB, опрос пошёл");
}

// Отправка из очереди: побайтно с паузой, если её просит профиль.
static void flushTx() {
    uint8_t buf[sizeof(s_tx)];
    size_t n;
    portENTER_CRITICAL(&s_txMux);
    n = s_txLen;
    memcpy(buf, s_tx, n);
    s_txLen = 0;
    portEXIT_CRITICAL(&s_txMux);
    if (!n || !s_ready) return;
    if (!INV_GAP_MS) { sendOut(buf, n); return; }
    for (size_t i = 0; i < n && s_ready && !s_gone; i++) {
        sendOut(buf + i, 1);
        pump(INV_GAP_MS);
    }
}

// ------------------------------------------------------------------ задачи

static void clientCb(const usb_host_client_event_msg_t* m, void*) {
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV)       s_newAddr = m->new_dev.address;
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) s_gone = true;
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
    cc.is_synchronous = false;
    cc.max_num_event_msg = 5;
    cc.async.client_event_callback = clientCb;
    if (usb_host_client_register(&cc, &s_client) != ESP_OK) {
        Serial.println("[инв] клиент USB-хоста не зарегистрировался");
        vTaskDelete(nullptr);
    }
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        if (s_gone) {
            s_gone = false;
            Serial.println("[инв] инвертор отключён от USB");
            closeDevice();
        }
        if (s_newAddr) {
            uint8_t a = s_newAddr;
            s_newAddr = 0;
            if (!s_dev) openDevice(a);
        }
        if (s_txLen) flushTx();
    }
}

bool invLooksLikeDongle(const String&, bool) { return false; }   // тут не ищут в эфире

void invTrBegin() {
    usb_host_config_t hc = {};
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
    if (usb_host_install(&hc) != ESP_OK) { Serial.println("[инв] USB-хост не поднялся"); return; }
    xTaskCreatePinnedToCore(hostTask,   "usbh",   3072, nullptr, 6, nullptr, 1);
    xTaskCreatePinnedToCore(clientTask, "usbinv", 6144, nullptr, 5, nullptr, 1);
    Serial.printf("[инв] транспорт USB, жду инвертор (%lu бод, пауза %u мс)\n",
                  (unsigned long)INV_BAUD, INV_GAP_MS);
}

bool invTrConnected() { return s_ready; }

bool invTrConnect() { return s_ready; }      // подключение ловит клиент сам, по событию

void invTrSend(const uint8_t* frame, size_t n) {
    if (n > sizeof(s_tx)) return;
    portENTER_CRITICAL(&s_txMux);
    memcpy(s_tx, frame, n);
    s_txLen = n;
    portEXIT_CRITICAL(&s_txMux);
}

void invBind(const String&) {
    Serial.println("[инв] по USB привязывать нечего: устройство одно");
}

#endif
#endif
