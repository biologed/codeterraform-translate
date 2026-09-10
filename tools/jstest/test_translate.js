/**
 * Тест скрипта перевода БЕЗ запуска игры.
 *
 * Скрипт перевода живёт внутри translate.cpp (как строковый литерал), и
 * проверить его иначе можно было только запуском игры руками. Здесь мы
 * достаём ровно тот же текст скрипта из исходника, подставляем в него
 * настоящий словарь и настоящий список защищённых зон из translations.json
 * и прогоняем на игрушечном DOM, похожем на интерфейс игры.
 *
 * Главное, что проверяется: перевод НЕ трогает то, что игрок написал сам
 * (код в редакторе, вывод его программы в консоли), но при этом переводит
 * ровно те же слова в интерфейсе.
 *
 * Запуск:  node test_translate.js
 */

const fs = require("fs");
const path = require("path");
const { JSDOM } = require("jsdom");

const DLL_DIR = path.join(__dirname, "..", "..", "dll");

// --- достаём скрипт и настройки из исходников -------------------------------

function buildScript() {
    const cpp = fs.readFileSync(path.join(DLL_DIR, "translate.cpp"), "utf8");

    // Скрипт собирается в C++ из трёх кусков: ...DICT = <json>; ...SKIP = <json>; ...
    const chunks = [...cpp.matchAll(/R"JS\(([\s\S]*?)\)JS"/g)].map((m) => m[1]);
    if (chunks.length !== 3) {
        throw new Error(`Ожидали 3 куска скрипта в translate.cpp, нашли ${chunks.length}`);
    }

    // Встроенный список защищённых зон - тоже из исходника, чтобы тест
    // проверял реальные значения, а не свою копию.
    const defaults = [];
    const block = cpp.match(/kDefaultSkipSelectors\[\]\s*=\s*\{([\s\S]*?)\n\s*\};/);
    for (const m of block[1].matchAll(/"((?:[^"\\]|\\.)*)"/g)) {
        defaults.push(JSON.parse(`"${m[1]}"`));
    }

    const config = readJsonWithComments(path.join(DLL_DIR, "translations.json"));
    const dict = config.translations ?? config;
    const skip = defaults.concat(config.settings?.skipSelectors ?? []);

    return {
        js: chunks[0] + JSON.stringify(dict) + chunks[1] + JSON.stringify(skip) + chunks[2],
        dict,
        skip,
    };
}

// translations.json намеренно разрешает комментарии (их понимает загрузчик
// в translate.cpp) - JSON.parse так не умеет, вырезаем их сами, аккуратно
// не тронув то, что внутри строк.
function readJsonWithComments(file) {
    const src = fs.readFileSync(file, "utf8");
    let out = "";
    let inString = false;
    for (let i = 0; i < src.length; i++) {
        const c = src[i];
        if (inString) {
            out += c;
            if (c === "\\") { out += src[++i]; continue; }
            if (c === '"') inString = false;
            continue;
        }
        if (c === '"') { inString = true; out += c; continue; }
        if (c === "/" && src[i + 1] === "/") {
            while (i < src.length && src[i] !== "\n") i++;
            out += "\n";
            continue;
        }
        if (c === "/" && src[i + 1] === "*") {
            i = src.indexOf("*/", i) + 1;
            continue;
        }
        out += c;
    }
    return JSON.parse(out);
}

// --- игрушечный интерфейс игры ---------------------------------------------

const PAGE = `
<body>
  <!-- интерфейс: это переводить НУЖНО -->
  <button id="ui-continue">Continue</button>
  <div id="ui-settings-label">Settings</div>
  <div class="docs-panel-root"><p id="docs-boot">boot</p></div>
  <p id="plain-boot">boot</p>
  <input id="search" placeholder="Back" type="text" value="Continue">

  <!-- то, что пишет игрок: это переводить НЕЛЬЗЯ -->
  <div class="cm-editor"><div class="cm-content"><span id="code-on">Continue</span></div></div>
  <textarea id="notes">Settings</textarea>
  <div class="console-output">
    <div class="console-line-output" id="console-user">boot</div>
    <div class="console-line-system" id="console-system">Continue</div>
  </div>
</body>`;

function run() {
    const { js, dict } = buildScript();
    const dom = new JSDOM(PAGE, { runScripts: "outside-only" });
    const { window } = dom;
    // jsdom не реализует innerText (в браузере он есть) - скрипт перевода
    // использует его только для диагностики, так что подменяем textContent.
    Object.defineProperty(window.HTMLElement.prototype, "innerText", {
        get() { return this.textContent; },
        configurable: true,
    });
    window.eval(js);
    return { window, dict };
}

// --- проверки ---------------------------------------------------------------

const results = [];
function check(name, actual, expected) {
    const ok = actual === expected;
    results.push({ ok, name, actual, expected });
}

const { window, dict } = run();
const $ = (sel) => window.document.querySelector(sel);
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

// Скрипт перевода стартует по DOMContentLoaded, а jsdom досылает это
// событие уже после конструктора - ждём его, иначе проверять будет нечего.
main();
async function main() {
await tick();

// 1. Обычный интерфейс переводится как и раньше.
check("кнопка меню переведена", $("#ui-continue").textContent, dict["Continue"]);
check("подпись переведена", $("#ui-settings-label").textContent, dict["Settings"]);
check("placeholder переведён", $("#search").getAttribute("placeholder"), dict["Back"]);

// 2. Пользовательский ввод не трогаем - это и была исходная проблема.
check("код в редакторе НЕ переведён", $("#code-on").textContent, "Continue");
check("textarea НЕ переведена", $("#notes").textContent, "Settings");
check("вывод программы игрока НЕ переведён", $("#console-user").textContent, "boot");
check("value поля ввода НЕ тронут", $("#search").getAttribute("value"), "Continue");

// 3. Сообщения самой игры в консоли переводятся (это не текст игрока).
check("системная строка консоли переведена", $("#console-system").textContent, dict["Continue"]);

// 4. Слово-команда переводится только в разделе помощи ("only" в словаре).
check("boot в разделе помощи переведён", $("#docs-boot").textContent, dict["boot"].to);
check("boot вне раздела помощи НЕ переведён", $("#plain-boot").textContent, "boot");

// 5. То, что игра дорисует позже (SPA), тоже должно попадать под те же правила.
const later = window.document.createElement("div");
later.textContent = "Continue";
window.document.body.appendChild(later);
const laterInEditor = window.document.createElement("div");
laterInEditor.textContent = "Continue";
$(".cm-content").appendChild(laterInEditor);

await tick();
check("новый узел интерфейса переведён", later.textContent, dict["Continue"]);
check("новый узел внутри редактора НЕ переведён", laterInEditor.textContent, "Continue");

// 6. Отладочные помощники, на которых держатся F7/F9.
const described = window.__translator.describe();
check("describe() отдаёт список зон", Array.isArray(described.skipSelectors), true);
const collected = window.__translator.collect();
check("collect() не тащит текст игрока", collected.includes("boot"), false);

report();
}

function report() {
    let failed = 0;
    for (const r of results) {
        if (r.ok) {
            console.log(`  ok   ${r.name}`);
        } else {
            failed++;
            console.log(`  FAIL ${r.name}\n         получили: ${JSON.stringify(r.actual)}` +
                        `\n         ожидали:  ${JSON.stringify(r.expected)}`);
        }
    }
    console.log(`\n${results.length - failed}/${results.length} проверок пройдено`);
    process.exit(failed ? 1 : 0);
}
