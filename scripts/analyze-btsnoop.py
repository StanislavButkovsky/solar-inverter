#!/usr/bin/env python3
"""Разбор ATT-трафика из btsnoop-лога: что приложение шлёт донглу инвертора.

Вход — выгрузка tshark:

    tshark -r btsnoop_hci.log -Y btatt.value \
      -T fields -e frame.time_relative -e btatt.opcode -e btatt.handle -e btatt.value \
      > att.tsv

    python3 scripts/analyze-btsnoop.py att.tsv

Скрипт отделяет команды от ответов, склеивает повторы и пытается опознать
протокол: ASCII Voltronic, Modbus RTU или неизвестное.
"""
import sys
from collections import Counter

# Коды операций ATT: чем отличается команда от ответа
OP = {
    0x12: "запись",            # Write Request
    0x52: "запись",            # Write Command
    0x1B: "уведомление",       # Handle Value Notification
    0x1D: "индикация",         # Handle Value Indication
    0x0B: "чтение",            # Read Response
}


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def looks_ascii(b):
    """Кадр Voltronic: печатаемый текст, два байта CRC и перевод строки."""
    if len(b) < 4 or b[-1] != 0x0D:
        return None
    body = b[:-3]
    if not body or not all(32 <= c < 127 for c in body):
        return None
    text = body.decode("ascii", "replace")
    ok = crc16_xmodem(body) == (b[-3] << 8 | b[-2])
    return text, ok


def looks_modbus(b):
    """Кадр Modbus RTU: адрес, функция, данные, CRC16 в конце."""
    if len(b) < 5:
        return None
    addr, func = b[0], b[1]
    if not (1 <= addr <= 247) or func not in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10):
        return None
    ok = crc16_modbus(b[:-2]) == (b[-1] << 8 | b[-2])
    return (addr, func), ok


def looks_daly(b):
    """На случай, если в эфире окажется знакомый кадр DALY."""
    return len(b) == 13 and b[0] == 0xA5


def main(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 4 or not parts[3]:
            continue
        t, op, handle, val = parts[0], parts[1], parts[2], parts[3]
        # у tshark значение приходит как "0a:1b:2c" либо сплошной строкой
        hexs = val.replace(":", "").replace(" ", "")
        try:
            data = bytes.fromhex(hexs)
        except ValueError:
            continue
        try:
            opcode = int(op, 0)
        except ValueError:
            opcode = -1
        rows.append((float(t or 0), opcode, handle, data))

    if not rows:
        print("В файле нет ATT-кадров с данными. Проверьте, что фильтр был btatt.value")
        return

    kinds = Counter()
    print("=" * 78)
    print("%-9s %-12s %-7s %s" % ("время", "направление", "хэндл", "содержимое"))
    print("=" * 78)

    seen = Counter()
    for t, opcode, handle, data in rows:
        kind = OP.get(opcode, "op 0x%02x" % opcode if opcode >= 0 else "?")

        note = ""
        a = looks_ascii(data)
        m = looks_modbus(data)
        if a:
            note = "ASCII Voltronic: «%s»%s" % (a[0], "" if a[1] else "  [CRC не сошлась]")
            kinds["ascii"] += 1
            seen[a[0].split("(")[0][:12]] += 1
        elif m:
            note = "Modbus RTU: адрес %d, функция 0x%02X%s" % (
                m[0][0], m[0][1], "" if m[1] else "  [CRC не сошлась]")
            kinds["modbus"] += 1
            seen["modbus f%02X" % m[0][1]] += 1
        elif looks_daly(data):
            note = "кадр DALY, DataID 0x%02X" % data[2]
            kinds["daly"] += 1
        else:
            printable = "".join(chr(c) if 32 <= c < 127 else "." for c in data)
            note = "%s   |%s|" % (data[:24].hex(" "), printable[:24])
            kinds["неопознано"] += 1

        print("%9.3f %-12s %-7s %s" % (t, kind, handle, note))

    print()
    print("=" * 78)
    print("Итого кадров: %d" % len(rows))
    for k, n in kinds.most_common():
        print("  %-12s %d" % (k, n))

    if seen:
        print("\nПовторяющиеся команды:")
        for k, n in seen.most_common(15):
            print("  %-16s %d раз" % (k, n))

    print()
    if kinds["ascii"]:
        print("ВЫВОД: инвертор говорит на ASCII-диалекте Voltronic.")
        print("       Реализуем QPIGS и родственные команды, CRC16/XMODEM.")
    elif kinds["modbus"]:
        print("ВЫВОД: инвертор говорит на Modbus RTU.")
        print("       Этап 2 сводится к чтению по известной карте регистров.")
    else:
        print("ВЫВОД: знакомых протоколов не найдено.")
        print("       Возможно, BLE донгла существует только ради настройки сети,")
        print("       и телеметрия туда не попадает. Тогда инвертор — только по RS485.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1])
