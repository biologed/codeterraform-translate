"""
Достаёт фронтенд (HTML/JS/CSS/шрифты) прямо из exe-шника Tauri-игры.

ЗАЧЕМ. Весь интерфейс игры - это веб-страница, а её файлы вшиты внутрь
exe и сжаты brotli, поэтому обычный поиск строк по бинарю (grep) не находит
ни одной фразы интерфейса. Раньше единственным способом собрать строки для
translations.json было ходить по игре и жать F9 на каждом экране. Этот
скрипт вытаскивает ВЕСЬ фронтенд целиком и сразу - включая тексты, которые
на экране появляются редко (ошибки, подсказки, разделы помощи).

КАК ЭТО УСТРОЕНО. Tauri на этапе сборки складывает ассеты в статическую
таблицу в секции .rdata. Каждая запись - 32 байта:

    [указатель на имя: 8][длина имени: 8][указатель на данные: 8][длина данных: 8]

Указатели - обычные виртуальные адреса (image_base + RVA), они лежат в файле
как есть. Найти таблицу можно так: находим в .rdata строку "/index.html"
(она есть в любой Tauri-сборке), считаем её виртуальный адрес и ищем в той же
секции 8 байт с этим адресом, за которыми идёт длина этой же строки - это и
есть начало одной из записей. Дальше идём от неё в обе стороны с шагом 32
байта, пока записи остаются валидными.

Идея подхода - из гиста liquidhelium (Rust, tauri-extract.rs):
https://gist.github.com/liquidhelium/0b03392e4e2ddd8c9600b2c89e55e300
Здесь переписано на Python (не нужен Rust-тулчейн), с 64-битными указателями
и более строгой проверкой записей.

ЗАПУСК:
    pip install brotli
    python extract_assets.py <путь к code-terraform.exe> [папка назначения]

Дальше см. dump_strings.py - он собирает из вытащенных .js/.html строки
интерфейса в заготовку словаря.
"""

import os
import re
import struct
import sys

try:
    import brotli
except ImportError:
    brotli = None


ENTRY_SIZE = 32          # ptr(8) + len(8) + ptr(8) + len(8)
MAX_NAME_LEN = 512
MAX_DATA_LEN = 256 * 1024 * 1024


def parse_pe(data):
    """Возвращает (image_base, [(имя, rva, raw_ptr, raw_size), ...])."""
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise SystemExit("Это не PE-файл")

    nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic == 0x20B:                                   # PE32+
        image_base = struct.unpack_from("<Q", data, opt_off + 24)[0]
    else:                                                # PE32
        image_base = struct.unpack_from("<I", data, opt_off + 28)[0]

    sections = []
    for i in range(nsec):
        off = opt_off + opt_size + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        _vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, rva, raw_ptr, raw_size))
    return image_base, sections


class Rdata:
    """Секция .rdata + перевод виртуальных адресов в смещения внутри неё."""

    def __init__(self, blob, rva_start, image_base):
        self.blob = blob
        self.rva_start = rva_start
        self.va_start = image_base + rva_start
        self.va_end = self.va_start + len(blob)

    def offset_of_va(self, va):
        if self.va_start <= va < self.va_end:
            return va - self.va_start
        return None

    def va_of_offset(self, off):
        return self.va_start + off


def find_table_start(rd, anchor=b"/index.html"):
    """Находит смещение записи таблицы, описывающей anchor."""
    name_off = rd.blob.find(anchor)
    if name_off < 0:
        raise SystemExit(f"Строка {anchor!r} в .rdata не найдена - "
                         f"похоже, это не Tauri-сборка (или другой якорь)")

    needle = struct.pack("<Q", rd.va_of_offset(name_off)) + struct.pack("<Q", len(anchor))
    start = 0
    while True:
        hit = rd.blob.find(needle, start)
        if hit < 0:
            raise SystemExit("Указатель на якорь в .rdata не найден - "
                             "формат таблицы ассетов, видимо, изменился")
        # Проверяем, что это действительно запись таблицы: сразу за именем
        # должен идти валидный указатель на данные.
        if read_entry(rd, hit) is not None:
            return hit
        start = hit + 1


def read_entry(rd, off):
    """Разбирает одну запись таблицы. None, если она невалидна."""
    if off < 0 or off + ENTRY_SIZE > len(rd.blob):
        return None

    name_va, name_len, data_va, data_len = struct.unpack_from("<QQQQ", rd.blob, off)
    if not (0 < name_len <= MAX_NAME_LEN) or not (0 < data_len <= MAX_DATA_LEN):
        return None

    name_off = rd.offset_of_va(name_va)
    data_off = rd.offset_of_va(data_va)
    if name_off is None or data_off is None:
        return None
    if name_off + name_len > len(rd.blob) or data_off + data_len > len(rd.blob):
        return None

    raw_name = rd.blob[name_off:name_off + name_len]
    try:
        name = raw_name.decode("utf-8")
    except UnicodeDecodeError:
        return None
    # Имя ассета - это путь вида "/assets/index-XXXX.js".
    if not name.startswith("/") or any(ord(c) < 0x20 for c in name):
        return None

    return name, rd.blob[data_off:data_off + data_len]


def decompress(data):
    """Ассеты сжаты brotli; шрифты/картинки иногда лежат как есть."""
    if brotli is None:
        return data, False
    try:
        return brotli.decompress(data), True
    except Exception:
        return data, False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    exe_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(os.path.abspath(exe_path)),
                     os.path.splitext(os.path.basename(exe_path))[0] + "_assets")

    if brotli is None:
        print("[!] Модуль brotli не установлен (pip install brotli) - "
              "файлы будут сохранены как есть, в сжатом виде")

    data = open(exe_path, "rb").read()
    image_base, sections = parse_pe(data)
    rdata = next((s for s in sections if s[0] == ".rdata"), None)
    if rdata is None:
        raise SystemExit("В файле нет секции .rdata")

    _, rva, raw_ptr, raw_size = rdata
    rd = Rdata(data[raw_ptr:raw_ptr + raw_size], rva, image_base)
    print(f"[*] image_base=0x{image_base:x}, .rdata rva=0x{rva:x}, {len(rd.blob):,} байт")

    start = find_table_start(rd)
    print(f"[*] Таблица ассетов найдена, запись /index.html на смещении 0x{start:x}")

    # Идём от найденной записи в обе стороны с шагом 32 байта.
    offsets = []
    off = start
    while read_entry(rd, off) is not None:
        offsets.append(off)
        off += ENTRY_SIZE
    off = start - ENTRY_SIZE
    while off >= 0 and read_entry(rd, off) is not None:
        offsets.append(off)
        off -= ENTRY_SIZE
    offsets.sort()

    os.makedirs(out_dir, exist_ok=True)
    written = raw_kept = 0
    total_bytes = 0
    for off in offsets:
        name, payload = read_entry(rd, off)
        content, unpacked = decompress(payload)
        if not unpacked:
            raw_kept += 1

        rel = name.lstrip("/")
        # Ассет не должен уметь писать за пределы папки назначения.
        dest = os.path.normpath(os.path.join(out_dir, rel))
        if not dest.startswith(os.path.normpath(out_dir) + os.sep):
            print(f"[!] Пропущено подозрительное имя: {name!r}")
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(content)
        written += 1
        total_bytes += len(content)

    print(f"[+] Извлечено файлов: {written} ({total_bytes:,} байт) -> {out_dir}")
    if raw_kept:
        print(f"    из них не сжаты brotli (сохранены как есть): {raw_kept}")

    interesting = sorted(
        (n for n in (read_entry(rd, o)[0] for o in offsets)
         if re.search(r"\.(js|mjs|html|css|json)$", n)),
    )
    print(f"[*] Текстовых файлов (js/html/css/json): {len(interesting)}")
    for n in interesting[:20]:
        print("   ", n)
    if len(interesting) > 20:
        print(f"    ... и ещё {len(interesting) - 20}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
