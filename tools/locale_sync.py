"""
Перекладывает перевод между рабочим файлом и файлом для репозитория.

ЗАЧЕМ ДВА ФАЙЛА. Переводить удобно в полном каталоге: рядом с ключом видно
английский оригинал. Но такой файл на 90% состоит из текста самой игры, и
класть его в публичный репозиторий не стоит - это чужой текст, а не наш
перевод, да и любая правка тонет в мегабайте неизменных строк.

Игре полный файл и не нужен: ключи, которых в каталоге нет, она показывает
по-английски сама (штатный фолбэк). Поэтому:

  locale/locale_ru.json       - только переведённые строки. Лежит в гите,
                                читается игрой, ревьюится по-человечески.
  locale/locale_ru.work.json  - полный каталог для работы. НЕ в гите,
                                собирается этой командой за секунду.

ЗАПУСК:

  # собрать рабочий файл (английский каталог + уже сделанный перевод)
  python locale_sync.py work <catalog.json> <locale_ru.json> <locale_ru.work.json>

  # выжать из рабочего файла только переведённое - перед коммитом
  python locale_sync.py export <locale_ru.work.json> <catalog.json> <locale_ru.json>

`catalog.json` - английский каталог, вывод tools/dump_strings.py.
"""

import json
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)


def count(catalog):
    return sum(len(v) for v in catalog.values() if isinstance(v, dict))


def make_work(catalog, translated):
    """Полный каталог, поверх которого положен готовый перевод."""
    out = {}
    for group, entries in catalog.items():
        out[group] = dict(entries)
        out[group].update(translated.get(group, {}))
    return out


def export(work, catalog):
    """Только то, что отличается от английского оригинала."""
    out, skipped = {}, 0
    for group, entries in work.items():
        original = catalog.get(group, {})
        kept = {}
        for key, text in entries.items():
            if key not in original:
                skipped += 1          # ключа больше нет в игре
                continue
            if text != original[key] and text.strip():
                kept[key] = text
        if kept:
            out[group] = kept
    return out, skipped


def main():
    args = sys.argv[1:]
    if len(args) != 4 or args[0] not in ("work", "export"):
        print(__doc__)
        return 1

    mode, first, second, target = args
    if mode == "work":
        catalog, translated = load(first), load(second)
        result = make_work(catalog, translated)
        save(target, result)
        print(f"[+] Рабочий файл собран: {target}\n"
              f"    строк всего {count(result)}, из них переведено {count(translated)}")
    else:
        work, catalog = load(first), load(second)
        result, skipped = export(work, catalog)
        save(target, result)
        print(f"[+] Файл для репозитория собран: {target}\n"
              f"    переведённых строк {count(result)} из {count(catalog)}"
              + (f"\n    пропущено ключей, которых больше нет в игре: {skipped}" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
