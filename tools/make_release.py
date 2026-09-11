"""
Собирает архив для игроков и считает SHA-256.

ЗАЧЕМ СЧИТАТЬ ХЕШИ. Мод внедряет DLL в чужой процесс, и антивирусы регулярно
поднимают на него ложную тревогу. Единственное, что можно этому
противопоставить без сертификата за деньги, - проверяемость: пользователь
должен иметь возможность убедиться, что скачанный файл в точности тот, что
выложил автор, и что собран он из открытого исходного кода. Поэтому рядом с
архивом всегда кладётся SHA256SUMS.txt, а сами сборки делает CI
(см. .github/workflows/build.yml).

ЗАПУСК (из корня проекта, после cmake --build):
    python tools/make_release.py [папка_сборки] [папка_назначения]

По умолчанию: build -> release/

В архив попадают: Translator.dll, TranslatorLauncher.exe, locale_ru.json,
"Запустить с переводом.bat", "ЧИТАЙ МЕНЯ.txt" и SHA256SUMS.txt.
"""

import hashlib
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "release")

    items = [
        (os.path.join(build_dir, "dll", "Release", "Translator.dll"), "Translator.dll"),
        (os.path.join(build_dir, "injector", "Release", "TranslatorLauncher.exe"), "TranslatorLauncher.exe"),
        (os.path.join(ROOT, "locale", "locale_ru.json"), "locale_ru.json"),
        (os.path.join(ROOT, "install", "Запустить с переводом.bat"), "Запустить с переводом.bat"),
        (os.path.join(ROOT, "install", "ЧИТАЙ МЕНЯ.txt"), "ЧИТАЙ МЕНЯ.txt"),
    ]

    missing = [src for src, _ in items if not os.path.isfile(src)]
    if missing:
        print("[!] Не найдены файлы (соберите проект: cmake --build build --config Release):")
        for path in missing:
            print("   ", path)
        return 1

    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)

    sums = []
    for src, name in items:
        shutil.copy2(src, os.path.join(out_dir, name))
        digest = sha256(src)
        sums.append(f"{digest}  {name}")
        print(f"[+] {name:32} {os.path.getsize(src):>9,} байт  {digest[:16]}...")

    sums_path = os.path.join(out_dir, "SHA256SUMS.txt")
    with open(sums_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(sums) + "\n")

    archive = os.path.join(out_dir, "CodeTerraform-RU.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for _src, name in items:
            zf.write(os.path.join(out_dir, name), name)
        zf.write(sums_path, "SHA256SUMS.txt")

    print(f"\n[+] Архив: {archive}")
    print(f"[+] Хеши:  {sums_path}")
    print("\nВ описание релиза имеет смысл вынести:")
    print("  * эти SHA-256 (чтобы их можно было сверить, не распаковывая архив);")
    print("  * строку про ложные срабатывания антивирусов и ссылку на исходники;")
    print("  * команду проверки происхождения сборки:")
    print("      gh attestation verify Translator.dll -R <владелец>/<репозиторий>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
