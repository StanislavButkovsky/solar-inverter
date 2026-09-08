// Общая часть профилей на Modbus: собрать запрос блока, склеить ответ,
// отдать разбор самому профилю. Компилируется для обоих инверторов
// на Modbus и не участвует в сборке под Voltronic.
#if !defined(TARGET_ASTERION)

#include "profile_modbus.h"

static MbAssembler s_asm;
static bool        s_inited = false;

void invResetStream() { s_asm.reset(); }

size_t invBuildRequest(uint8_t step, uint8_t* out, size_t cap) {
    if (cap < 8) return 0;
    if (!s_inited) {
        s_asm.begin(INV_SLAVE, INV_PREFIX, INV_PREFIX_LEN);
        s_inited = true;
    }
    const InvBlock& b = INV_BLOCKS[step % INV_NSTEPS];
    mbBuildRead(out, INV_SLAVE, b.start, b.count);
    return 8;
}

bool invFeed(uint8_t step, const uint8_t* data, size_t n) {
    const uint8_t* words;
    uint8_t nwords;
    if (!s_asm.feed(data, n, &words, &nwords)) return false;
    invApplyBlock(INV_BLOCKS[step % INV_NSTEPS].start, words, nwords);
    return true;
}

#endif
