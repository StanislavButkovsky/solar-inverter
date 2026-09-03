# Пре-сборочный шаг: web/index.html → src/webui_gz.h (gzip в PROGMEM).
#
# Зачем: страница занимала ~34 КБ флеша текстом и жила внутри C++-строки, где
# её неудобно править и легко сломать экранированием. Теперь это обычный html,
# а в прошивку уезжает сжатая копия — браузеры распаковывают gzip сами.
import gzip, os

Import("env")  # noqa

SRC = os.path.join(env.subst("$PROJECT_DIR"), "web", "index.html")   # noqa
DST = os.path.join(env.subst("$PROJECT_DIR"), "src", "webui_gz.h")   # noqa


def build():
    raw = open(SRC, "rb").read()
    # mtime=0: иначе каждая сборка даёт другие байты и файл вечно «изменён»
    packed = gzip.compress(raw, 9, mtime=0)

    out = [
        "// Сгенерировано scripts/gen_web.py из web/index.html — не править руками.",
        "// Страница отдаётся как есть, с заголовком Content-Encoding: gzip.",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "static const size_t INDEX_HTML_RAW_LEN = %d;" % len(raw),
        "static const uint8_t INDEX_HTML_GZ[] PROGMEM = {",
    ]
    for i in range(0, len(packed), 16):
        out.append("    " + ", ".join("0x%02x" % b for b in packed[i:i + 16]) + ",")
    out.append("};")
    out.append("static const size_t INDEX_HTML_GZ_LEN = sizeof(INDEX_HTML_GZ);")
    text = "\n".join(out) + "\n"

    old = open(DST).read() if os.path.exists(DST) else ""
    if old != text:
        open(DST, "w").write(text)
    print("страница: %d байт -> %d байт gzip (%.0f%%)"
          % (len(raw), len(packed), 100.0 * len(packed) / len(raw)))


build()
