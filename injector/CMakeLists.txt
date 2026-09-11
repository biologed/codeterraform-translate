# Лаунчер: запускает игру и внедряет в неё Translator.dll.
#
# ПРО ИМЯ ФАЙЛА. Раньше он назывался injector.exe. Имя честное, но ровно так
# же называют себя вредоносные программы, и человек, у которого антивирус
# поднял тревогу, читает "injector" как подтверждение худших опасений.
# Файл делает ровно одно: запускает игру с переводом, - поэтому и называется
# соответственно. На технику внедрения это, разумеется, не влияет (она
# описана в README), но снимает лишний повод для паники.
add_executable(injector injector.cpp)
target_compile_features(injector PRIVATE cxx_std_17)

set_target_properties(injector PROPERTIES OUTPUT_NAME "TranslatorLauncher")

add_version_info(injector
    "Запуск Code: Terraform с русским переводом"
    "TranslatorLauncher.exe"
    VFT_APP)
