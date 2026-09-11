cmake_minimum_required(VERSION 3.20)
project(Translator VERSION 1.0.0 LANGUAGES CXX)

# Всё собранное складываем в одну папку build/bin, без вложенности по целям и
# конфигурациям. По умолчанию Visual Studio раскидывает результат по
# build/<цель>/<конфигурация>/, и чтобы собрать релиз, файлы приходится
# выковыривать из трёх разных мест - при том что в папку игры они всё равно
# кладутся рядом друг с другом.
#
# Переменные с суффиксом конфигурации нужны обязательно: без них
# многоконфигурационные генераторы (как раз Visual Studio) всё равно добавят
# к пути Release/ или Debug/.
#
# ВНИМАНИЕ: раз путь один на все конфигурации, Debug перезапишет Release в той
# же папке. Если нужны обе сразу - делайте отдельные каталоги сборки
# (cmake -B build-debug).
set(TRANSLATOR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TRANSLATOR_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${TRANSLATOR_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${TRANSLATOR_OUTPUT_DIR}")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${TRANSLATOR_OUTPUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${TRANSLATOR_OUTPUT_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${TRANSLATOR_OUTPUT_DIR}")
endforeach()

# Кто и откуда - попадает в свойства собранных файлов (см. cmake/version.rc.in).
# ПЕРЕД ВЫПУСКОМ РЕЛИЗА подставьте сюда свой репозиторий: пользователь,
# у которого антивирус поднял тревогу, первым делом смотрит свойства файла,
# и ссылка на открытый исходный код там - самый полезный ответ.
set(TRANSLATOR_AUTHOR "Code: Terraform RU" CACHE STRING "Автор/сообщество перевода")
set(TRANSLATOR_URL "https://github.com/biologed/codeterraform-translate" CACHE STRING "Ссылка на исходный код")
set(TRANSLATOR_PRODUCT_NAME "Русификатор Code: Terraform")

# Добавляет цели ресурс с описанием файла: версия, назначение, автор, ссылка
# на исходники. Голый безымянный бинарник - лишний повод для эвристик
# антивируса и для недоверия пользователя; подписи это не заменяет, но
# убирает часть поводов.
function(add_version_info target description original_name filetype)
    set(TRANSLATOR_VERSION "${PROJECT_VERSION}")
    set(TRANSLATOR_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(TRANSLATOR_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    set(TRANSLATOR_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    set(TRANSLATOR_RC_DESCRIPTION "${description}")
    set(TRANSLATOR_RC_ORIGINAL_NAME "${original_name}")
    set(TRANSLATOR_RC_INTERNAL_NAME "${target}")
    set(TRANSLATOR_RC_FILETYPE "${filetype}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/version.rc.in" "${generated}" @ONLY)
    target_sources(${target} PRIVATE "${generated}")
endfunction()

add_subdirectory(dll)
add_subdirectory(injector)
