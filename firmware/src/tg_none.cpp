// Сборка без бота Telegram (NO_TG): тот же интерфейс, что у tg.cpp, но ничего
// не шлёт и в сеть не ходит. Владельцу первого устройства бот не нужен, а
// его задача (стек 12 КБ) и TLS-рукопожатие (~40 КБ пиком) — самое крупное в
// памяти, которой у этой платы с двумя соединениями Bluetooth впритык.
#if defined(NO_TG)

#include "tg.h"

void   tgStart() {}
void   tgLoop() {}
void   tgNotify(const String&) {}
void   tgOutage(bool, uint32_t) {}
// absent — страница прячет вкладку Telegram.
String tgStateJson() { return "{\"absent\":true,\"on\":false,\"chat\":false,\"low\":20,\"net\":false,\"err\":\"\"}"; }
String tgSetToken(const String&) { return "бот в этой сборке выключен (NO_TG)"; }
void   tgUnbind() {}
void   tgTest() {}
String tgReport(uint8_t) { return "бот в этой сборке выключен (NO_TG) — сводки не собираются"; }

#endif
