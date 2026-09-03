#include "state.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static BmsState        g_state;
static InvState        g_inv;
static SemaphoreHandle_t g_mtx = nullptr;

void stateInit() {
    g_mtx = xSemaphoreCreateMutex();
}

BmsState stateSnapshot() {
    BmsState copy;
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        copy = g_state;
        xSemaphoreGive(g_mtx);
    }
    return copy;
}

void stateUpdate(const std::function<void(BmsState&)>& fn) {
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        fn(g_state);
        xSemaphoreGive(g_mtx);
    }
}

InvState invSnapshot() {
    InvState copy;
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        copy = g_inv;
        xSemaphoreGive(g_mtx);
    }
    return copy;
}

void invUpdate(const std::function<void(InvState&)>& fn) {
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        fn(g_inv);
        xSemaphoreGive(g_mtx);
    }
}
