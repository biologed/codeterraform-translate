#include "translate.h"
#include "hooks.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <vector>
#include <string>

using json = nlohmann::json;

namespace {

    // Компактный JSON-объект словаря, например: {"Start Game":"Начать игру"}.
    // JSON-объект с виде "ключ": "значение" - это одновременно валидный
    // литерал JS-объекта, поэтому его можно вставить в скрипт как есть.
    std::string g_dictJsonMinified = "{}";

    // JSON-массив CSS-селекторов "зон пользовательского ввода" - то, что
    // переводить НЕЛЬЗЯ (см. большой комментарий у kDefaultSkipSelectors).
    std::string g_skipSelectorsJson = "[]";

    bool g_loadedOnce = false;

    // ЗАЧЕМ ЭТО ВООБЩЕ НУЖНО.
    //
    // Перевод работает по DOM целиком: MutationObserver ловит любой новый
    // текстовый узел и заменяет его по словарю. Но в этой игре пользователь
    // сам ПИШЕТ ТЕКСТ - код в редакторе и команды в консоли. Его ввод
    // попадает в тот же DOM обычными текстовыми узлами, и словарь начинает
    // "переводить" то, что напечатал игрок: набрал в редакторе ON - на
    // экране стало ВКЛ, и код перестал работать. По той же причине нельзя
    // было добавить в словарь слово boot (нужное в разделе помощи) - оно
    // ломало ввод команды boot в консоли.
    //
    // Отсюда правило: сначала определяем, находится ли узел в зоне
    // пользовательского ввода, и только потом решаем, переводить ли его.
    // Список ниже - защита "из коробки" (стандартные поля ввода и популярные
    // веб-редакторы кода/терминалы), к нему добавляются селекторы из секции
    // settings.skipSelectors в translations.json - конкретные классы
    // редактора и консоли этой игры подбираются хоткеем F7 и НЕ требуют
    // пересборки DLL.
    const char* const kDefaultSkipSelectors[] = {
        // Служебные теги: их содержимое - не текст интерфейса.
        "script", "style", "noscript",
        // Штатные поля ввода. Текст внутри <textarea> - это ЕГО значение,
        // то есть буквально то, что напечатал пользователь.
        "textarea", "input",
        // Редактируемые области (так делают почти все веб-редакторы кода).
        "[contenteditable]:not([contenteditable=\"false\"])",
        "[role=\"textbox\"]",
        // Популярные редакторы кода и эмуляторы терминала - на случай, если
        // игра использует один из них (классы у них стабильные).
        ".cm-editor", ".cm-content", ".CodeMirror",
        ".monaco-editor", ".ace_editor",
        ".xterm", ".xterm-screen",
        // Ручные пометки "не переводить" - можно проставить их самому через
        // settings.skipSelectors, либо использовать стандартный HTML-атрибут.
        "[data-no-translate]", "[translate=\"no\"]", ".notranslate", ".no-translate",
    };

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
        return w;
    }

    void SetDefaultSkipSelectors(std::vector<std::string>& out)
    {
        for (const char* sel : kDefaultSkipSelectors) out.push_back(sel);
    }

} // namespace

namespace Translate {

void Reload()
{
    g_loadedOnce = true;

    std::vector<std::string> skipSelectors;
    SetDefaultSkipSelectors(skipSelectors);
    g_skipSelectorsJson = json(skipSelectors).dump();

    std::wstring path = GetDllFolder() + L"\\translations.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        // Это НЕ ошибка: словарь по DOM - вспомогательный механизм. Основной
        // перевод (locale_patch, см. assets.cpp) работает от него независимо,
        // и при переводе через локаль игры translations.json обычно не нужен
        // вовсе. Формулировка важна: раньше здесь было "перевод отключён", и
        // это читалось как "ничего не работает".
        WriteLog(L"[translate] translations.json рядом с DLL нет - словарь по DOM выключен "
                 L"(для перевода через locale_patch он не нужен)");
        g_dictJsonMinified = "{}";
        return;
    }

    try {
        // ignore_comments = true: позволяем писать в translations.json
        // комментарии (// ...) - файл правится руками, и пометки вроде
        // "это в разделе помощи" там очень к месту.
        json j = json::parse(file, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
        if (!j.is_object()) {
            WriteLog(L"[translate] translations.json должен быть объектом вида {\"оригинал\": \"перевод\"}");
            g_dictJsonMinified = "{}";
            return;
        }

        // Поддерживаем ДВА формата файла:
        //  1) плоский  {"оригинал": "перевод"}                  - как было раньше;
        //  2) с настройками {"settings": {...}, "translations": {...}}.
        // Второй нужен, чтобы список зон пользовательского ввода
        // (skipSelectors) правился без пересборки DLL, рядом со словарём.
        const json* dict = &j;
        if (j.contains("translations") && j["translations"].is_object()) {
            dict = &j["translations"];

            if (j.contains("settings") && j["settings"].is_object()) {
                const json& settings = j["settings"];

                if (settings.value("useDefaultSkipSelectors", true) == false) {
                    skipSelectors.clear();
                }
                if (settings.contains("skipSelectors") && settings["skipSelectors"].is_array()) {
                    for (const auto& sel : settings["skipSelectors"]) {
                        if (sel.is_string() && !sel.get<std::string>().empty()) {
                            skipSelectors.push_back(sel.get<std::string>());
                        }
                    }
                }
                g_skipSelectorsJson = json(skipSelectors).dump();
            }
        }

        g_dictJsonMinified = dict->dump();

        size_t scoped = 0;
        for (auto it = dict->begin(); it != dict->end(); ++it) {
            if (it.value().is_object()) scoped++;
        }
        WriteLog(L"[translate] Загружено строк перевода: " + std::to_wstring(dict->size())
                 + L" (из них с ограничением по разделу: " + std::to_wstring(scoped) + L")");
        WriteLog(L"[translate] Зон, защищённых от перевода (пользовательский ввод): "
                 + std::to_wstring(skipSelectors.size()));
    } catch (const std::exception& e) {
        WriteLog(L"[translate] Ошибка разбора translations.json: " + Utf8ToWide(e.what()));
        g_dictJsonMinified = "{}";
    }
}

std::wstring BuildBootstrapScript()
{
    if (!g_loadedOnce) {
        Reload();
    }

    // Собираем скрипт в UTF-8 (так удобнее писать строки на русском прямо
    // в C++ коде), а в конце конвертируем в UTF-16 под WebView2 API.
    //
    // ВАЖНО: скрипт в конце возвращает JSON-диагностику (url, readyState,
    // сколько узлов реально заменил самый первый синхронный проход walk(),
    // количество iframe'ов). Это не нужно для AddScriptToExecuteOnDocumentCreated
    // (там результат никто не читает), но критично для "живого" вызова
    // через ExecuteScript (F11/перезагрузка словаря без рестарта игры) -
    // именно так мы смогли увидеть, ЧТО реально произошло на уже открытой
    // странице, вместо гадания.
    //
    // Кроме самого перевода скрипт публикует наружу window.__translator
    // с двумя отладочными функциями - collect() и describe(). Ими
    // пользуются хоткеи F7/F9 из webview.cpp: логика "что считается
    // пользовательским вводом" живёт здесь в одном месте, и дампы обязаны
    // использовать ровно её же, иначе они будут показывать не то, что
    // реально переводится.
    std::string js = R"JS(
(function() {
    var DICT = )JS" + g_dictJsonMinified + R"JS(;
    var SKIP = )JS" + g_skipSelectorsJson + R"JS(;

    // Один общий селектор быстрее, чем перебор списка в цикле на каждом узле.
    var SKIP_SELECTOR = SKIP.join(',');

    // Чтобы не переводить один и тот же текстовый узел повторно и не
    // зациклиться через собственный MutationObserver.
    var translated = new WeakSet();
    var replacedCount = 0;
    var skippedUserInput = 0;

    function elementOf(node) {
        if (!node) return null;
        return node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
    }

    // closest() с защитой от кривого селектора в translations.json: одна
    // опечатка в skipSelectors не должна ронять весь перевод целиком.
    function closest(el, selector) {
        if (!el || !selector || !el.closest) return null;
        try { return el.closest(selector); } catch (e) { return null; }
    }

    function isEditable(el) {
        if (!el) return false;
        var tag = el.tagName;
        if (tag === 'INPUT' || tag === 'TEXTAREA') return true;
        return !!el.isContentEditable;
    }

    // ГЛАВНАЯ ЗАЩИТА: текст, который написал сам игрок (код в редакторе,
    // команды в консоли), переводить нельзя - иначе набранное ON превратится
    // в ВКЛ прямо в его коде. Проверяем двумя независимыми способами, чтобы
    // защита работала даже до того, как в settings.skipSelectors подобраны
    // точные классы редактора этой игры:
    //   1) узел внутри одной из известных зон ввода (список SKIP);
    //   2) узел внутри элемента, в котором пользователь ПРЯМО СЕЙЧАС печатает
    //      (document.activeElement и он редактируемый) - это ловит любой
    //      редактор, даже с неизвестной нам разметкой.
    function isUserInput(node) {
        var el = elementOf(node);
        if (!el) return false;

        if (closest(el, SKIP_SELECTOR)) return true;

        var active = document.activeElement;
        if (active && isEditable(active) &&
            (active === el || (active.contains && active.contains(el)))) {
            return true;
        }
        return false;
    }

    // Для атрибутов защита мягче: placeholder/title/aria-label - это подписи
    // ОТ игры, а не текст пользователя, их у поля ввода как раз хочется
    // переводить. Уважаем только явные пометки "не переводить".
    function isNoTranslateZone(el) {
        return !!closest(el, '[data-no-translate],[translate="no"],.notranslate,.no-translate');
    }

    // Значение в словаре - это либо просто строка перевода, либо объект
    //   {"to": "перевод", "only": "CSS-селектор", "except": "CSS-селектор"}
    // Форма с "only" решает вторую половину проблемы: слово вроде boot нужно
    // перевести В РАЗДЕЛЕ ПОМОЩИ, но оставить как есть везде, где игрок
    // может его набрать сам. Тогда пишем:
    //   "boot": {"to": "загрузка", "only": ".help-panel"}
    // и перевод применится только внутри указанного контейнера.
    function resolve(entry, node) {
        if (entry === undefined || entry === null) return undefined;
        if (typeof entry === 'string') return entry;
        if (typeof entry !== 'object') return undefined;

        var el = elementOf(node);
        if (entry.only && !closest(el, entry.only)) return undefined;
        if (entry.except && closest(el, entry.except)) return undefined;
        return typeof entry.to === 'string' ? entry.to : undefined;
    }

    function applyToTextNode(node) {
        if (!node || !node.nodeValue || translated.has(node)) return;
        var trimmed = node.nodeValue.trim();
        if (!trimmed) return;
        if (DICT[trimmed] === undefined) return;   // дешёвая проверка ДО closest()
        if (isUserInput(node)) { skippedUserInput++; return; }

        var replacement = resolve(DICT[trimmed], node);
        if (replacement !== undefined) {
            node.nodeValue = node.nodeValue.replace(trimmed, replacement);
            translated.add(node);
            replacedCount++;
        }
    }

    function applyToAttributes(el) {
        if (!el || !el.getAttribute || isNoTranslateZone(el)) return;

        ['placeholder', 'title', 'aria-label', 'alt'].forEach(function(attr) {
            var v = el.getAttribute(attr);
            if (!v) return;
            var replacement = resolve(DICT[v.trim()], el);
            if (replacement !== undefined) {
                el.setAttribute(attr, replacement);
                replacedCount++;
            }
        });

        // Атрибут value трогаем ТОЛЬКО у кнопок, где это подпись. У обычного
        // <input> value - это содержимое поля, то есть потенциально текст,
        // который ввёл игрок; переводить его нельзя ни при каких условиях.
        if (el.tagName === 'INPUT' && /^(button|submit|reset)$/i.test(el.getAttribute('type') || '')) {
            var v = el.getAttribute('value');
            if (v) {
                var replacement = resolve(DICT[v.trim()], el);
                if (replacement !== undefined) {
                    el.setAttribute('value', replacement);
                    replacedCount++;
                }
            }
        }
    }

    // "Смешанный" контент - когда фраза разбита на несколько DOM-узлов,
    // например: 'вызовите через <code>self</code>.' - это ОДИН текстовый
    // узел до <code>, элемент <code> с текстом внутри, и ещё один текстовый
    // узел после. Обычный applyToTextNode такое не поймает вообще - у него
    // каждый кусок по отдельности либо не имеет смысла как отдельная фраза,
    // либо случайно совпадёт с чем-то не тем. Вместо этого строим
    // "нормализованную" сигнатуру всего родителя (теги БЕЗ атрибутов - в
    // словаре ключ пишется как <code>self</code>, а не с реальными
    // style="..." из разметки, которые смотреть в dump_html.txt неудобно
    // и незачем) и ищем её целиком в словаре.
    function hasMixedContent(el) {
        var hasText = false, hasElement = false;
        for (var i = 0; i < el.childNodes.length; i++) {
            var node = el.childNodes[i];
            if (node.nodeType === Node.TEXT_NODE && node.nodeValue.trim()) hasText = true;
            if (node.nodeType === Node.ELEMENT_NODE) hasElement = true;
        }
        return hasText && hasElement;
    }

    // Схлопываем только пробелы/табы - переносы строк (\n) СОХРАНЯЕМ.
    // Некоторые элементы содержат целый список пунктов внутри ОДНОГО
    // текстового узла (перенос строки не через <br>, а обычным \n + CSS
    // white-space) - переносы нужны как границы между пунктами, чтобы
    // applyMixedContent ниже могла пробовать переводить их ПО ОДНОМУ, а не
    // только весь блок целиком одной гигантской строкой в словаре.
    function normalizedSignature(el) {
        var parts = [];
        for (var i = 0; i < el.childNodes.length; i++) {
            var node = el.childNodes[i];
            if (node.nodeType === Node.TEXT_NODE) {
                parts.push(node.nodeValue);
            } else if (node.nodeType === Node.ELEMENT_NODE) {
                var tag = node.tagName.toLowerCase();
                parts.push('<' + tag + '>' + node.textContent + '</' + tag + '>');
            }
        }
        return parts.join('')
            .replace(/[ \t]+/g, ' ')
            .split('\n').map(function(line) { return line.trim(); }).join('\n')
            .trim();
    }

    // Значение в словаре для такого ключа - тоже HTML с ТЕМИ ЖЕ тегами в
    // том же порядке (например: '...через <code>self</code>.'). Атрибуты/
    // стили при этом берём из ОРИГИНАЛЬНЫХ элементов страницы (чтобы не
    // потерять оформление вроде рамки у "self only") - из шаблона перевода
    // используется только текст внутри тегов и текст между ними.
    function applyMixedContent(el) {
        if (!el || !el.childNodes || translated.has(el) || !hasMixedContent(el)) return;
        if (isUserInput(el)) { skippedUserInput++; return; }

        var signature = normalizedSignature(el);
        if (!signature) return;
        var replacement = resolve(DICT[signature], el);

        // Целиком блок в словаре не нашёлся - и в нём есть переносы строк
        // (несколько пунктов списка внутри одного элемента)? Пробуем
        // перевести КАЖДУЮ строку по отдельности - так словарь не
        // раздувается одной огромной строкой на весь абзац, а пункты можно
        // добавлять по одному. Строки, для которых перевода ещё нет,
        // остаются как есть (на следующем F11 их можно доперевести).
        if (replacement === undefined && signature.indexOf('\n') !== -1) {
            var lines = signature.split('\n');
            var anyLineMatched = false;
            var translatedLines = lines.map(function(line) {
                var lineReplacement = resolve(DICT[line], el);
                if (lineReplacement !== undefined) {
                    anyLineMatched = true;
                    return lineReplacement;
                }
                return line;
            });
            if (anyLineMatched) {
                replacement = translatedLines.join('\n');
            }
        }

        if (replacement === undefined) return;

        var tmp = document.createElement('div');
        tmp.innerHTML = replacement;

        var origElements = [], templateElements = [];
        for (var i = 0; i < el.childNodes.length; i++) {
            if (el.childNodes[i].nodeType === Node.ELEMENT_NODE) origElements.push(el.childNodes[i]);
        }
        for (var j = 0; j < tmp.childNodes.length; j++) {
            if (tmp.childNodes[j].nodeType === Node.ELEMENT_NODE) templateElements.push(tmp.childNodes[j]);
        }
        for (var k = 0; k < origElements.length && k < templateElements.length; k++) {
            origElements[k].textContent = templateElements[k].textContent;
        }

        var templateTexts = [];
        for (var m = 0; m < tmp.childNodes.length; m++) {
            if (tmp.childNodes[m].nodeType === Node.TEXT_NODE) templateTexts.push(tmp.childNodes[m].nodeValue);
        }
        var ti = 0;
        for (var n = 0; n < el.childNodes.length; n++) {
            if (el.childNodes[n].nodeType === Node.TEXT_NODE && ti < templateTexts.length) {
                el.childNodes[n].nodeValue = templateTexts[ti++];
            }
        }

        translated.add(el);
        replacedCount++;
    }

    function walk(root) {
        if (!root) return;

        if (root.nodeType === Node.TEXT_NODE) {
            applyToTextNode(root);
            return;
        }
        if (root.nodeType === Node.ELEMENT_NODE) {
            // Целую поддеревню-зону ввода (редактор, консоль) обходим
            // стороной сразу, не спускаясь внутрь - и быстрее, и надёжнее.
            if (closest(root, SKIP_SELECTOR)) { skippedUserInput++; return; }

            applyToAttributes(root);
            // "Смешанный" контент проверяем и применяем ДО обычного
            // текстового прохода ниже - иначе к моменту, когда мы дойдём
            // сюда, вложенные текстовые узлы (например "self" внутри
            // <code>) уже могут быть переведены по отдельности, и сигнатура
            // всего родителя перестанет совпадать с оригинальным ключом.
            applyMixedContent(root);
            if (root.querySelectorAll) {
                root.querySelectorAll('*').forEach(applyMixedContent);
            }
        }

        var walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, null);
        var n;
        while ((n = walker.nextNode())) applyToTextNode(n);

        if (root.querySelectorAll) {
            root.querySelectorAll('[placeholder],[title],[aria-label],[alt],input[type="button"],input[type="submit"],input[type="reset"]')
                .forEach(applyToAttributes);
        }
    }

    // ---- отладочные помощники для F7/F9 (см. webview.cpp) ----------------

    function cssPath(el) {
        var parts = [], cur = el, guard = 0;
        while (cur && cur.nodeType === Node.ELEMENT_NODE && guard++ < 12) {
            var s = cur.tagName.toLowerCase();
            if (cur.id) s += '#' + cur.id;
            if (cur.classList && cur.classList.length) {
                s += '.' + Array.prototype.slice.call(cur.classList).join('.');
            }
            parts.unshift(s);
            cur = cur.parentElement;
        }
        return parts.join(' > ');
    }

    // Описание одного элемента: путь до него, классы и - главное - какой
    // именно селектор из SKIP его сейчас защищает (или null, если никакой).
    // Именно по этому полю понятно, надо ли дописывать что-то в
    // settings.skipSelectors, чтобы игра перестала переводить ввод игрока.
    function describeElement(el) {
        if (!el) return null;
        var matched = null;
        for (var i = 0; i < SKIP.length; i++) {
            if (closest(el, SKIP[i])) { matched = SKIP[i]; break; }
        }
        return {
            path: cssPath(el),
            tag: el.tagName ? el.tagName.toLowerCase() : null,
            id: el.id || null,
            classes: el.classList ? Array.prototype.slice.call(el.classList) : [],
            editable: isEditable(el),
            protectedFromTranslation: isUserInput(el),
            matchedSkipSelector: matched,
            suggestedSkipSelector: (el.id ? '#' + el.id
                : (el.classList && el.classList.length ? '.' + el.classList[0] : null)),
            textSample: (el.textContent || '').trim().slice(0, 120)
        };
    }

    // Собирает строки-кандидаты на перевод с ТЕКУЩЕГО экрана, честно
    // пропуская зоны пользовательского ввода и уже переведённые фразы.
    function collect(includeAlreadyInDict) {
        var seen = Object.create(null), out = [];
        function push(s) {
            if (!s) return;
            s = String(s).trim();
            if (!s || seen[s]) return;
            if (!includeAlreadyInDict && DICT[s] !== undefined) return;
            seen[s] = true;
            out.push(s);
        }
        if (!document.body) return out;

        var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null);
        var n;
        while ((n = walker.nextNode())) {
            if (isUserInput(n)) continue;
            push(n.nodeValue);
        }
        document.body.querySelectorAll('*').forEach(function(el) {
            if (hasMixedContent(el) && !isUserInput(el)) push(normalizedSignature(el));
            if (isNoTranslateZone(el)) return;
            ['placeholder', 'title', 'aria-label', 'alt'].forEach(function(attr) {
                if (el.getAttribute) push(el.getAttribute(attr));
            });
        });
        return out;
    }

    window.__translator = {
        collect: collect,
        skipSelectors: SKIP,
        dictSize: Object.keys(DICT).length,
        describe: function() {
            return {
                url: String(location.href),
                skipSelectors: SKIP,
                active: describeElement(document.activeElement),
                hovered: describeElement(window.__translatorLastHover || null)
            };
        }
    };

    // Запоминаем последний элемент под курсором: у консоли/лога обычно нет
    // фокуса, и без этого F7 про них ничего бы не рассказал - достаточно
    // навести мышь и нажать F7.
    if (!window.__translatorHoverHooked) {
        window.__translatorHoverHooked = true;
        document.addEventListener('mouseover', function(e) {
            window.__translatorLastHover = e.target;
        }, true);
    }

    // ---------------------------------------------------------------------

    function start() {
        if (!document.body) return;

        walk(document.body);

        // Tauri-приложения - это SPA, контент часто меняется без полной
        // перезагрузки страницы. Следим за изменениями и переводим их тоже.
        // Старый наблюдатель отключаем: скрипт запускается заново на каждый
        // F11, и без этого после нескольких перезагрузок словаря на странице
        // висело бы несколько MutationObserver'ов сразу.
        if (window.__translatorObserver) {
            try { window.__translatorObserver.disconnect(); } catch (e) {}
        }
        var observer = new MutationObserver(function(mutations) {
            mutations.forEach(function(m) {
                if (m.type === 'characterData') {
                    applyToTextNode(m.target);
                } else if (m.type === 'childList') {
                    m.addedNodes.forEach(walk);
                }
            });
        });
        observer.observe(document.body, {
            childList: true,
            subtree: true,
            characterData: true
        });
        window.__translatorObserver = observer;
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', start);
    } else {
        start();
    }

    return JSON.stringify({
        url: String(location.href),
        readyState: document.readyState,
        // Язык, который игра выставила себе сама. Если подменой файлов
        // (locale_patch) в неё добавлена русская локаль, здесь будет ru -
        // это самый быстрый способ проверить, что подмена сработала.
        documentLang: document.documentElement ? document.documentElement.lang : null,
        hasBody: !!document.body,
        bodyTextLength: document.body ? document.body.innerText.length : -1,
        frames: window.frames ? window.frames.length : -1,
        dictSize: Object.keys(DICT).length,
        skipSelectors: SKIP.length,
        replacedOnFirstPass: replacedCount,
        skippedUserInputOnFirstPass: skippedUserInput
    });
})();
)JS";

    return Utf8ToWide(js);
}

} // namespace Translate
