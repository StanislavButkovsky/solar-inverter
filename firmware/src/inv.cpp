// Опрос инвертора: общая часть для обеих версий устройства.
//
// Здесь только цикл «спросить блок — дождаться ответа — разложить в снапшот».
// Что за провод под этим и какие регистры читать — знают inv_transport
// и profile соответственно.
#include "inv.h"
#include "inv_transport.h"
#include "profile.h"
#include "state.h"
#include "cfg.h"
#include "events.h"

static const uint32_t STALE_MS = 20000;
static const uint32_t POLL_MS  = 2000;
static const uint32_t RETRY_MS = 8000;

static uint8_t     s_step = 0;
static uint32_t    s_lastPoll = 0, s_lastTry = 0;
static bool        s_wasConnected = false;

void invOnBytes(const uint8_t* data, size_t n) {
    // Профиль сам решает, собрался ли ответ: у Modbus это длина и контрольная
    // сумма, у Voltronic — возврат каретки в конце строки.
    if (invFeed(s_step, data, n))
        s_step = (s_step + 1) % INV_NSTEPS;
}

void invStart() {
    invUpdate([](InvState& v) { v = InvState(); });
    invResetStream();
    invTrBegin();
    Serial.printf("[инв] профиль: %s, шагов опроса %u\n", INV_PROFILE_NAME, INV_NSTEPS);
}

void invTick() {
    bool up = invTrConnected();

    if (up != s_wasConnected) {
        s_wasConnected = up;
        evAdd(up ? EV_INV_UP : EV_INV_DOWN);
        if (!up) {
            invResetStream();
            invUpdate([](InvState& v) { v.linked = false; v.fresh = false; });
        }
    }

    if (!up) {
        if (millis() - s_lastTry > RETRY_MS) {
            s_lastTry = millis();
            invTrConnect();
        }
        return;
    }

    if (millis() - s_lastPoll < POLL_MS) return;
    s_lastPoll = millis();

    uint8_t req[32];
    size_t n = invBuildRequest(s_step, req, sizeof(req));
    invResetStream();
    if (n) invTrSend(req, n);

    invUpdate([](InvState& v) {
        if (v.lastFrameMs && millis() - v.lastFrameMs > STALE_MS) v.fresh = false;
    });
}
