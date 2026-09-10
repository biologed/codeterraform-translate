#pragma once
#include <windows.h>
#include <string>

// Простое логирование в translator_log.txt рядом с самой DLL.
// Т.к. у игры обычно нет консоли, это единственный способ увидеть,
// что вообще происходит внутри хуков.
void WriteLog(const std::wstring& message);

// Папка, в которой лежит наша DLL (без завершающего слэша).
// Используется для translator_log.txt, dump.txt и translations.json.
std::wstring GetDllFolder();

// Конвертирует "широкую" (UTF-16) строку в UTF-8. Нужно, чтобы писать
// русский (и вообще любой не-ASCII) текст в файлы: обычный std::wofstream
// без этого обрезает текст на первом же не-ASCII символе.
std::string WideToUtf8(const std::wstring& wide);

namespace Hooks {
    // Ставит хук на GetProcAddress - БЫСТРО и СИНХРОННО. Вызывается
    // напрямую из DllMain (DLL_PROCESS_ATTACH), а не из фонового потока -
    // это критично для тайминга, см. подробный комментарий в hooks.cpp
    // прямо перед реализацией. Возвращает false, если что-то пошло не так
    // (MH_Initialize/MH_CreateHook/MH_EnableHook не удались).
    bool InstallEarly();

    // Остальная, не срочная часть: диагностика и подстраховка на случай
    // attach-режима. Вызывается один раз из отдельного потока (см.
    // dllmain.cpp) - ПОСЛЕ того, как InstallEarly() уже отработала.
    void Install();
}
