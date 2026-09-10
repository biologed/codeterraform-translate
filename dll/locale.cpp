#include "locale.h"
#include "hooks.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <cctype>
#include <fstream>
#include <mutex>
#include <string>

using json = nlohmann::json;

namespace {

    std::mutex g_mutex;
    std::string g_declarations;    // готовый кусок JS с каталогом
    size_t g_keyCount = 0;

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
        return w;
    }

    // --- разбор минифицированного JS ------------------------------------
    //
    // Ищем ФОРМУ функции поиска перевода, а не её имя: после минификации
    // имена в каждой сборке игры новые, а форма остаётся:
    //
    //     function w(e,t){let n=p.get(t);return n?n.get(e)??null:null}
    //
    // Отсюда нам нужно имя первого параметра (ключ) и позиция начала тела.

    struct Cursor {
        const std::string& s;
        size_t i;

        void skipWs() { while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i; }

        bool literal(const char* text)
        {
            skipWs();
            size_t len = strlen(text);
            if (s.compare(i, len, text) != 0) return false;
            i += len;
            return true;
        }

        bool ident(std::string& out)
        {
            skipWs();
            size_t start = i;
            while (i < s.size()) {
                char c = s[i];
                bool ok = isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
                if (i == start && isdigit(static_cast<unsigned char>(c))) return false;
                if (!ok) break;
                ++i;
            }
            if (i == start) return false;
            out = s.substr(start, i - start);
            return true;
        }
    };

    // Проверяет форму, начиная с позиции слова function. При успехе
    // возвращает имя параметра-ключа и позицию сразу после "{" тела.
    bool MatchLookupFunction(const std::string& src, size_t at,
                             std::string& keyArg, size_t& bodyStart)
    {
        Cursor c{ src, at };
        std::string fnName, argKey, argLang, local, map, tmp;

        if (!c.literal("function")) return false;
        if (!c.ident(fnName)) return false;
        if (!c.literal("(")) return false;
        if (!c.ident(argKey)) return false;
        if (!c.literal(",")) return false;
        if (!c.ident(argLang)) return false;
        if (!c.literal(")")) return false;
        if (!c.literal("{")) return false;

        bodyStart = c.i;

        if (!c.literal("let")) return false;
        if (!c.ident(local)) return false;
        if (!c.literal("=")) return false;
        if (!c.ident(map)) return false;
        if (!c.literal(".get(")) return false;
        if (!c.ident(tmp) || tmp != argLang) return false;
        if (!c.literal(")")) return false;
        if (!c.literal(";")) return false;
        if (!c.literal("return")) return false;
        if (!c.ident(tmp) || tmp != local) return false;
        if (!c.literal("?")) return false;
        if (!c.ident(tmp) || tmp != local) return false;
        if (!c.literal(".get(")) return false;
        if (!c.ident(tmp) || tmp != argKey) return false;
        if (!c.literal(")")) return false;
        if (!c.literal("??")) return false;
        if (!c.literal("null")) return false;
        if (!c.literal(":")) return false;
        if (!c.literal("null")) return false;
        if (!c.literal("}")) return false;

        keyArg = argKey;
        return true;
    }

    // Находит функцию поиска перевода. Отталкиваемся от её хвоста
    // "??null:null}" - он достаточно редкий, чтобы кандидатов было мало, -
    // и от каждого кандидата отходим назад к ближайшему "function".
    bool FindLookupFunction(const std::string& src, std::string& keyArg,
                            size_t& fnStart, size_t& bodyStart)
    {
        static const std::string kTail = "??null:null}";
        size_t pos = 0;
        while ((pos = src.find(kTail, pos)) != std::string::npos) {
            size_t windowStart = pos > 200 ? pos - 200 : 0;
            size_t candidate = src.rfind("function", pos);
            if (candidate != std::string::npos && candidate >= windowStart) {
                if (MatchLookupFunction(src, candidate, keyArg, bodyStart)) {
                    fnStart = candidate;
                    return true;
                }
            }
            pos += kTail.size();
        }
        return false;
    }

} // namespace

namespace Locale {

size_t Reload()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_declarations.clear();
    g_keyCount = 0;

    std::wstring path = GetDllFolder() + L"\\locale_ru.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        WriteLog(L"[locale] locale_ru.json рядом с DLL нет - перевод через локаль игры выключен");
        return 0;
    }

    json catalog;
    try {
        catalog = json::parse(file, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        WriteLog(L"[locale] Ошибка разбора locale_ru.json: " + Utf8ToWide(e.what()));
        return 0;
    }
    if (!catalog.is_object()) {
        WriteLog(L"[locale] locale_ru.json должен быть объектом вида "
                 L"{\"группа\": {\"ключ\": \"текст\"}}");
        return 0;
    }

    // Плоский вид "группа.ключ" -> текст: именно такими ключами игра
    // спрашивает перевод (её собственная развёртка каталога делает то же).
    json flat = json::object();
    size_t skipped = 0;
    for (auto group = catalog.begin(); group != catalog.end(); ++group) {
        if (!group.value().is_object()) continue;
        for (auto entry = group.value().begin(); entry != group.value().end(); ++entry) {
            if (!entry.value().is_string()) continue;
            const std::string text = entry.value().get<std::string>();
            // Пустой перевод - это не перевод: пусть игра покажет оригинал.
            if (text.find_first_not_of(" \t\r\n") == std::string::npos) { skipped++; continue; }
            flat[group.key() + "." + entry.key()] = text;
        }
    }

    g_keyCount = flat.size();
    if (g_keyCount == 0) {
        WriteLog(L"[locale] В locale_ru.json нет ни одной непустой строки");
        return 0;
    }

    // Безусловная отметка "наш патч исполнился" - по ней потом видно, что
    // страница выполнила именно наш файл (см. диагностику в webview.cpp).
    g_declarations =
        "try{(typeof globalThis!=='undefined'?globalThis:this).__TR_PATCH_LOADED="
        "((typeof globalThis!=='undefined'?globalThis:this).__TR_PATCH_LOADED||0)+1;}catch(e){}"
        "var __TR_FLAT=" + flat.dump() + ",__TR_KEYS=" + std::to_string(g_keyCount)
        + ",__trMarked=0;";

    WriteLog(L"[locale] locale_ru.json загружен, строк перевода: " + std::to_wstring(g_keyCount)
             + (skipped ? L" (пустых пропущено: " + std::to_wstring(skipped) + L")" : L""));
    return g_keyCount;
}

bool IsLoaded()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_declarations.empty();
}

bool PatchJs(const std::string& source, std::string& out)
{
    std::string declarations;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_declarations.empty()) return false;
        declarations = g_declarations;
    }

    std::string keyArg;
    size_t fnStart = 0, bodyStart = 0;
    if (!FindLookupFunction(source, keyArg, fnStart, bodyStart)) return false;

    // Отметку об успехе ставим один раз, а не на каждый ключ: через эту
    // функцию проходят тысячи обращений за кадр.
    const std::string injection =
        "var __trV=typeof __TR_FLAT!=='undefined'?__TR_FLAT[" + keyArg + "]:undefined;"
        "if(__trV!==undefined){"
        "if(!__trMarked){__trMarked=1;try{(typeof globalThis!=='undefined'?globalThis:this)"
        ".__TR_LOCALE_APPLIED={keys:__TR_KEYS,at:new Date().toISOString()};}catch(e){}}"
        "return __trV;}";

    out.reserve(source.size() + declarations.size() + injection.size());
    out.assign(source, 0, fnStart);          // до функции
    out += declarations;                     // каталог - в ту же область видимости
    out.append(source, fnStart, bodyStart - fnStart);
    out += injection;                        // врезка - в начало тела
    out.append(source, bodyStart, std::string::npos);
    return true;
}

} // namespace Locale
