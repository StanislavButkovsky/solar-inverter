// Модуль батареи для сборок без Bluetooth (флаг NO_BLE: HS35, Asterion).
//
// У этих инверторов батарея штатная и говорит через сам инвертор, а владелец
// Bluetooth не использует (21.10.2026). Стек NimBLE в такой сборке только ел
// бы флеш и память, поэтому его нет вовсе. Остаётся то, ради чего задача
// вообще существует, — цикл опроса инвертора, исторически живущий здесь
// (в daly.cpp он делит задачу с опросом батареи по BLE). Поиск и привязка
// батареи честно отвечают, что Bluetooth нет.
#if defined(NO_BLE)

#include "daly.h"
#include "inv.h"
#include "cfg.h"

static void task(void*) {
    invStart();
    for (;;) {
        invTick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void dalyStart() { xTaskCreatePinnedToCore(task, "inv", 6144, nullptr, 5, nullptr, 0); }

void dalySetBle(bool) { }
bool dalyBle() { return false; }

void     dalyReleaseFor(uint32_t) { }
bool     dalyIsReleased() { return false; }
uint32_t dalyReleaseLeftS() { return 0; }

std::vector<BleFound> dalyScan(uint8_t) { return {}; }
void dalyRequestScan() { }
bool dalyScanBusy() { return false; }
std::vector<BleFound> dalyScanResults() { return {}; }

void dalyBind(const String&) {
    Serial.println("[ble] в этой сборке Bluetooth нет — батарея читается через инвертор");
}

void dalyParseFrame(const uint8_t*) { }

#endif
