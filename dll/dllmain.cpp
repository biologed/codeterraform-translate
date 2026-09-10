#include "hooks.h"
#include <windows.h>

// Тяжёлая/долгая часть (ожидание, диагностика) уходит в отдельный поток -
// делать ЕЁ прямо в DllMain было бы плохой идеей: пока DllMain не вернёт
// управление, процесс держит "loader lock", и Sleep/WaitFor* тут держать
// нельзя.
//
// А вот САМ хук (Hooks::InstallEarly) - наоборот, вызывается СИНХРОННО
// прямо здесь, до создания потока и до возврата из DllMain. Это важно:
// инжектор вызывает LoadLibraryW -> (тут выполняется весь DllMain) ->
// только потом ResumeThread на основном потоке игры. Если бы установка
// хука была отложена в фоновый поток, возникала бы гонка: основной поток
// игры (уже отпущенный по ResumeThread) мог бы дойти до нужного вызова
// быстрее, чем наш фоновый поток вообще успевал начать выполняться.
// Синхронный вызов здесь эту гонку исключает полностью - основной поток
// игры в этот момент физически ещё не запущен.
static DWORD WINAPI MainThread(LPVOID)
{
    Hooks::Install();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // Нам не нужны уведомления о создании/завершении потоков игры -
        // отключаем их, это стандартная практика для DLL, которые не
        // делают ничего специфичного для потоков.
        DisableThreadLibraryCalls(hModule);
        WriteLog(L"[dllmain] DLL загружена в процесс");
        Hooks::InstallEarly();
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        WriteLog(L"[dllmain] DLL выгружается");
        break;
    }
    return TRUE;
}
