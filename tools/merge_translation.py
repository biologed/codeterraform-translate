"""
Вливает порцию перевода в locale_ru.json и проверяет её.

Перевод удобно делать частями: файл большой, а игра показывает
непереведённые ключи по-английски сама (штатный фолбэк), поэтому его можно
наполнять постепенно и играть уже сейчас. Этот скрипт принимает порцию в
виде {"группа.ключ": "перевод"} и аккуратно кладёт её на место.

ЧТО ПРОВЕРЯЕТСЯ (молча сломать перевод легко, поэтому лучше поймать сразу):
  * ключ существует в английском каталоге - иначе это опечатка, и строка
    никогда не покажется;
  * подстановки совпадают: если в оригинале {count}, он должен остаться и в
    переводе, иначе игрок увидит дыру вместо числа;
  * перевод непустой.

ЗАПУСК:
    python merge_translation.py <locale_ru.json> <порция.json> [--check <catalog.json>]
    python merge_translation.py <locale_ru.json> --progress [--check <catalog.json>]

--progress ничего не меняет, только показывает, сколько уже переведено.
--check указывает на английский каталог (вывод dump_strings.py); если не
указан, ищется catalog.json рядом с locale-файлом.
"""

import json
import os
import re
import sys

PLACEHOLDER = re.compile(r"\{[a-zA-Z_][a-zA-Z0-9_]*\}")


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)


def flat(catalog):
    return {f"{g}.{k}": v
            for g, entries in catalog.items()
            for k, v in entries.items()}


def find_english(locale_path, explicit):
    if explicit:
        return load(explicit)
    guess = os.path.join(os.path.dirname(os.path.abspath(locale_path)), "catalog.json")
    if os.path.isfile(guess):
        return load(guess)
    return None


def progress(locale, english):
    """Переведённой считается строка, которая отличается от английской."""
    total = translated = 0
    by_group = {}
    for group, entries in locale.items():
        done = 0
        for key, value in entries.items():
            total += 1
            original = english.get(group, {}).get(key) if english else None
            if original is not None and value != original:
                translated += 1
                done += 1
        by_group[group] = (done, len(entries))
    return total, translated, by_group


def main():
    args = sys.argv[1:]
    check = None
    if "--check" in args:
        i = args.index("--check")
        check = args[i + 1]
        del args[i:i + 2]

    show_progress = "--progress" in args
    if show_progress:
        args.remove("--progress")

    if not args:
        print(__doc__)
        return 1

    locale_path = args[0]
    locale = load(locale_path)
    english = find_english(locale_path, check)

    if not show_progress:
        if len(args) < 2:
            print(__doc__)
            return 1

        batch = load(args[1])
        english_flat = flat(english) if english else {}
        applied, problems = 0, []

        for full_key, text in batch.items():
            group, _, key = full_key.partition(".")
            if not key:
                problems.append(f"{full_key}: ключ должен быть вида группа.ключ")
                continue
            if not isinstance(text, str) or not text.strip():
                problems.append(f"{full_key}: пустой перевод")
                continue
            if english_flat and full_key not in english_flat:
                problems.append(f"{full_key}: такого ключа в игре нет (опечатка?)")
                continue
            if english_flat:
                want = sorted(PLACEHOLDER.findall(english_flat[full_key]))
                got = sorted(PLACEHOLDER.findall(text))
                if want != got:
                    problems.append(f"{full_key}: подстановки не совпадают "
                                    f"({' '.join(want) or '—'} -> {' '.join(got) or '—'})")
                    continue
            locale.setdefault(group, {})[key] = text
            applied += 1

        for p in problems:
            print(f"[!] {p}")
        if applied:
            save(locale_path, locale)
        print(f"[+] Влито строк: {applied}"
              + (f", отклонено: {len(problems)}" if problems else ""))

    if english:
        total, done, by_group = progress(locale, english)
        print(f"\n[*] Переведено: {done} из {total} строк ({done * 100 // max(total, 1)}%)")
        pending = sorted(((n - d, g) for g, (d, n) in by_group.items() if d < n), reverse=True)
        print("    Крупнейшее непереведённое:")
        for left, group in pending[:8]:
            print(f"      {group:24} осталось {left}")
    else:
        print("[*] Английский каталог не найден - прогресс посчитать не могу "
              "(укажите --check catalog.json)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
