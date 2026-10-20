// Строка для JSON: кавычки, обратная косая и управляющие символы экранируются.
//
// Без этого одно имя сети в кавычках — «подключаюсь к "Keenetic"» — делало
// весь ответ /api/state невалидным, страница не могла его разобрать и
// показывала пустой дашборд (20.10.2026, сразу после того как задали Wi-Fi).
#pragma once
#include <Arduino.h>

inline String jsonEsc(const String& s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((uint8_t)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
