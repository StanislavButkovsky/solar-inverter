// Опрос инвертора: общая часть для обеих версий устройства.
//
// Здесь только цикл «спросить блок — дождаться ответа — разложить в снапшот».
// Что за провод под этим и какие регистры читать — знают inv_transport
// и profile соответственно.
#include "inv.h"
#include "inv_transport.h"
#include "profile.h"
#include "modbus.h"
#include "state.h"
#include "cfg.h"
#include "events.h"

static const uint32_t STALE_MS = 20000;
static const uint32_t POLL_MS  = 2000;
static const uint32_t RETRY_MS = 8000;

static MbAssembler s_asm;
static uint8_t     s_block = 0;
static uint32_t    s_lastPoll = 0, s_lastTry = 0;
static bool        s_wasConnected = false;

void invOnBytes(const uint8_t* data, size_t n) {
    const uint8_t* words;
    uint8_t nwords;
    if (!s_asm.feed(data, n, &words, &nwords)) return;

    invApplyBlock(INV_BLOCKS[s_block].start, words, nwords);
    s_block = (s_block + 1) % INV_NBLOCKS;
}

void invStart() {
    invUpdate([](InvState& v) { v = InvState(); });
    s_asm.begin(INV_SLAVE, INV_PREFIX, INV_PREFIX_LEN);
    invTrBegin();
    Serial.printf("[инв] профиль: %s, адрес %u, блоков %u\n",
                  INV_PROFILE_NAME, INV_SLAVE, INV_NBLOCKS);
}

void invTick() {
    bool up = invTrConnected();

    if (up != s_wasConnected) {
        s_wasConnected = up;
        evAdd(up ? EV_INV_UP : EV_INV_DOWN);
        if (!up) {
            s_asm.reset();
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

    uint8_t req[8];
    mbBuildRead(req, INV_SLAVE, INV_BLOCKS[s_block].start, INV_BLOCKS[s_block].count);
    s_asm.reset();
    invTrSend(req, sizeof(req));

    invUpdate([](InvState& v) {
        if (v.lastFrameMs && millis() - v.lastFrameMs > STALE_MS) v.fresh = false;
    });
}
