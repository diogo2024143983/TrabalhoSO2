#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHAVE_REGISTO _T("Software\\TrabSO2")
#define VALOR_NPIPE _T("NPIPE")
#define EVENTO_NOTIFICAR _T("notificar")
#define TAM_MAX_COMANDO 256

typedef struct {
    BYTE tipo;      // 4 - novo alerta
    TCHAR msg[140];
    DWORD duracao;  // segundos
} MSG_ALERTA;

typedef struct {
    HANDLE eventoParar;
    HANDLE eventoNotificar;
    CRITICAL_SECTION csEscrita;
    TCHAR nomePipe[256];
    volatile LONG ligado;
    volatile LONG deveSair;
} CONTEXTO_APP;

static void ImprimirComTimestamp(CONTEXTO_APP* contexto, const TCHAR* texto) {
    SYSTEMTIME tempoSistema;
    GetLocalTime(&tempoSistema);

    EnterCriticalSection(&contexto->csEscrita);
    _tprintf(
        _T("%02u/%02u/%04u (%02u:%02u:%02u): '%s'\n"),
        tempoSistema.wDay, tempoSistema.wMonth, tempoSistema.wYear,
        tempoSistema.wHour, tempoSistema.wMinute, tempoSistema.wSecond,
        texto
    );
    LeaveCriticalSection(&contexto->csEscrita);
}

static void ImprimirAlertaComDuracao(CONTEXTO_APP* contexto, const TCHAR* msg, DWORD duracaoSeg) {
    SYSTEMTIME tempoSistema;
    GetLocalTime(&tempoSistema);

    EnterCriticalSection(&contexto->csEscrita);
    _tprintf(
        _T("%02u/%02u/%04u (%02u:%02u:%02u): '%s' (%lu segundos)\n"),
        tempoSistema.wDay, tempoSistema.wMonth, tempoSistema.wYear,
        tempoSistema.wHour, tempoSistema.wMinute, tempoSistema.wSecond,
        msg,
        (unsigned long)duracaoSeg
    );
    LeaveCriticalSection(&contexto->csEscrita);
}

static BOOL GarantirNomePipeNoRegisto(const TCHAR* daLinhaComandos, TCHAR* nomePipeSaida, DWORD tamanhoSaida) {
    HKEY chave = NULL;
    LONG resultado;
    DWORD tipo = 0;
    DWORD bytesDados = tamanhoSaida * sizeof(TCHAR);

    resultado = RegCreateKeyEx(
        HKEY_CURRENT_USER,
        CHAVE_REGISTO,
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        NULL,
        &chave,
        NULL
    );
    if (resultado != ERROR_SUCCESS) {
        _tprintf(_T("Erro: nao foi possivel aceder ao Registry (RegCreateKeyEx=%ld)\n"), resultado);
        return FALSE;
    }

    if (daLinhaComandos != NULL && daLinhaComandos[0] != _T('\0')) {
        _tcsncpy_s(nomePipeSaida, tamanhoSaida, daLinhaComandos, _TRUNCATE);
        resultado = RegSetValueEx(
            chave,
            VALOR_NPIPE,
            0,
            REG_SZ,
            (const BYTE*)nomePipeSaida,
            (DWORD)((_tcslen(nomePipeSaida) + 1) * sizeof(TCHAR))
        );
        if (resultado != ERROR_SUCCESS) {
            _tprintf(_T("Erro: nao foi possivel guardar NPIPE no Registry (RegSetValueEx=%ld)\n"), resultado);
            RegCloseKey(chave);
            return FALSE;
        }
        RegCloseKey(chave);
        return TRUE;
    }

    resultado = RegQueryValueEx(chave, VALOR_NPIPE, NULL, &tipo, (LPBYTE)nomePipeSaida, &bytesDados);
    RegCloseKey(chave);

    if (resultado != ERROR_SUCCESS || tipo != REG_SZ || nomePipeSaida[0] == _T('\0')) {
        _tprintf(_T("[ERRO] O nome do 'NPIPE' nao foi especificado (args ou registry)!\n"));
        return FALSE;
    }
    return TRUE;
}

static BOOL LerAlertaDoRegisto(const TCHAR* nomeValor, MSG_ALERTA* alertaSaida) {
    HKEY chave = NULL;
    LONG resultado;
    DWORD tipo = 0;
    DWORD bytesDados = sizeof(MSG_ALERTA);

    resultado = RegOpenKeyEx(HKEY_CURRENT_USER, CHAVE_REGISTO, 0, KEY_READ, &chave);
    if (resultado != ERROR_SUCCESS) {
        return FALSE;
    }

    ZeroMemory(alertaSaida, sizeof(*alertaSaida));
    resultado = RegQueryValueEx(chave, nomeValor, NULL, &tipo, (LPBYTE)alertaSaida, &bytesDados);
    RegCloseKey(chave);

    if (resultado != ERROR_SUCCESS || tipo != REG_BINARY || bytesDados != sizeof(MSG_ALERTA)) {
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI ThreadAlertas(LPVOID parametro) {
    CONTEXTO_APP* contexto = (CONTEXTO_APP*)parametro;
    HANDLE esperasAguardarNotificacao[2] = { contexto->eventoParar, contexto->eventoNotificar };

    while (InterlockedCompareExchange(&contexto->deveSair, 0, 0) == 0) {
        DWORD resultadoEspera = WaitForMultipleObjects(2, esperasAguardarNotificacao, FALSE, INFINITE);
        if (resultadoEspera == WAIT_OBJECT_0) {
            break;
        }
        if (resultadoEspera != WAIT_OBJECT_0 + 1) {
            continue;
        }

        if (InterlockedCompareExchange(&contexto->deveSair, 0, 0) != 0) {
            break;
        }

        for (;;) {
            if (InterlockedCompareExchange(&contexto->deveSair, 0, 0) != 0) {
                return 0;
            }

            if (!ResetEvent(contexto->eventoNotificar)) {
                EnterCriticalSection(&contexto->csEscrita);
                _tprintf(_T("Aviso: falha no ResetEvent(notificar) (%lu)\n"), GetLastError());
                LeaveCriticalSection(&contexto->csEscrita);
                break;
            }

            MSG_ALERTA alerta;
            if (!LerAlertaDoRegisto(contexto->nomePipe, &alerta)) {
                EnterCriticalSection(&contexto->csEscrita);
                _tprintf(_T("Aviso: nao foi possivel ler alerta BINARY no valor '%s'.\n"), contexto->nomePipe);
                LeaveCriticalSection(&contexto->csEscrita);
                break;
            }

            if (alerta.tipo != 4) {
                break;
            }

            alerta.msg[_countof(alerta.msg) - 1] = _T('\0');
            ImprimirAlertaComDuracao(contexto, alerta.msg, alerta.duracao);

            HANDLE temporizador = CreateWaitableTimer(NULL, TRUE, NULL);
            if (temporizador == NULL) {
                EnterCriticalSection(&contexto->csEscrita);
                _tprintf(_T("Aviso: CreateWaitableTimer falhou (%lu)\n"), GetLastError());
                LeaveCriticalSection(&contexto->csEscrita);
                break;
            }

            LARGE_INTEGER instanteDisparo;
            instanteDisparo.QuadPart = -((LONGLONG)alerta.duracao * 10000000LL);
            if (!SetWaitableTimer(temporizador, &instanteDisparo, 0, NULL, NULL, FALSE)) {
                CloseHandle(temporizador);
                EnterCriticalSection(&contexto->csEscrita);
                _tprintf(_T("Aviso: SetWaitableTimer falhou (%lu)\n"), GetLastError());
                LeaveCriticalSection(&contexto->csEscrita);
                break;
            }

            HANDLE esperasDuranteAlerta[3] = {
                contexto->eventoParar,
                contexto->eventoNotificar,
                temporizador
            };
            resultadoEspera = WaitForMultipleObjects(3, esperasDuranteAlerta, FALSE, INFINITE);
            CloseHandle(temporizador);

            if (resultadoEspera == WAIT_OBJECT_0) {
                return 0;
            }
            if (resultadoEspera == WAIT_OBJECT_0 + 2) {
                ImprimirComTimestamp(contexto, _T("---"));
                break;
            }
            if (resultadoEspera == WAIT_OBJECT_0 + 1) {
                continue;
            }
            break;
        }
    }
    return 0;
}

static DWORD WINAPI ThreadComandos(LPVOID parametro) {
    CONTEXTO_APP* contexto = (CONTEXTO_APP*)parametro;
    TCHAR comando[TAM_MAX_COMANDO];

    while (InterlockedCompareExchange(&contexto->deveSair, 0, 0) == 0) {
        EnterCriticalSection(&contexto->csEscrita);
        _tprintf(_T("CMD> "));
        LeaveCriticalSection(&contexto->csEscrita);

        if (_fgetts(comando, TAM_MAX_COMANDO, stdin) == NULL) {
            InterlockedExchange(&contexto->deveSair, 1);
            SetEvent(contexto->eventoParar);
            SetEvent(contexto->eventoNotificar);
            break;
        }

        size_t tamanho = _tcslen(comando);
        while (tamanho > 0 && (comando[tamanho - 1] == _T('\n') || comando[tamanho - 1] == _T('\r'))) {
            comando[--tamanho] = _T('\0');
        }

        if (_tcsicmp(comando, _T("liga")) == 0) {
            if (InterlockedCompareExchange(&contexto->ligado, 0, 0) != 0) {
                EnterCriticalSection(&contexto->csEscrita);
                _tprintf(_T("Placar ja se encontra ligado.\n"));
                LeaveCriticalSection(&contexto->csEscrita);
                continue;
            }

            DWORD identificador = (DWORD)((rand() % 999) + 1);
            InterlockedExchange(&contexto->ligado, 1);
            EnterCriticalSection(&contexto->csEscrita);
            _tprintf(_T("Identificador = %lu\n"), identificador);
            LeaveCriticalSection(&contexto->csEscrita);
        } else if (_tcsicmp(comando, _T("desliga")) == 0) {
            EnterCriticalSection(&contexto->csEscrita);
            _tprintf(_T("A terminar...\n"));
            LeaveCriticalSection(&contexto->csEscrita);

            InterlockedExchange(&contexto->deveSair, 1);
            SetEvent(contexto->eventoParar);
            SetEvent(contexto->eventoNotificar);
            break;
        } else if (comando[0] != _T('\0')) {
            EnterCriticalSection(&contexto->csEscrita);
            _tprintf(_T("Comando invalido. Use: liga | desliga\n"));
            LeaveCriticalSection(&contexto->csEscrita);
        }
    }
    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
    CONTEXTO_APP contexto;
    ZeroMemory(&contexto, sizeof(contexto));
    InitializeCriticalSection(&contexto.csEscrita);

    srand((unsigned int)time(NULL));

    const TCHAR* argumentoPipe = NULL;
    if (argc >= 2 && argv[1] != NULL && argv[1][0] != _T('\0')) {
        argumentoPipe = argv[1];
    }

    if (!GarantirNomePipeNoRegisto(argumentoPipe, contexto.nomePipe, _countof(contexto.nomePipe))) {
        DeleteCriticalSection(&contexto.csEscrita);
        return 1;
    }

    EnterCriticalSection(&contexto.csEscrita);
    _tprintf(_T("Named Pipe = '%s'\n"), contexto.nomePipe);
    LeaveCriticalSection(&contexto.csEscrita);

    contexto.eventoParar = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (contexto.eventoParar == NULL) {
        DeleteCriticalSection(&contexto.csEscrita);
        _tprintf(_T("Erro: CreateEvent(stop) falhou (%lu)\n"), GetLastError());
        return 1;
    }

    contexto.eventoNotificar = CreateEvent(NULL, TRUE, FALSE, EVENTO_NOTIFICAR);
    if (contexto.eventoNotificar == NULL) {
        CloseHandle(contexto.eventoParar);
        DeleteCriticalSection(&contexto.csEscrita);
        _tprintf(_T("Erro: CreateEvent(notificar) falhou (%lu)\n"), GetLastError());
        return 1;
    }

    HANDLE threadComandos = CreateThread(NULL, 0, ThreadComandos, &contexto, 0, NULL);
    HANDLE threadAlertas = CreateThread(NULL, 0, ThreadAlertas, &contexto, 0, NULL);
    if (threadComandos == NULL || threadAlertas == NULL) {
        EnterCriticalSection(&contexto.csEscrita);
        _tprintf(_T("Erro: falha a criar threads.\n"));
        LeaveCriticalSection(&contexto.csEscrita);
        InterlockedExchange(&contexto.deveSair, 1);
        SetEvent(contexto.eventoParar);
        SetEvent(contexto.eventoNotificar);
        if (threadComandos != NULL) CloseHandle(threadComandos);
        if (threadAlertas != NULL) CloseHandle(threadAlertas);
        CloseHandle(contexto.eventoNotificar);
        CloseHandle(contexto.eventoParar);
        DeleteCriticalSection(&contexto.csEscrita);
        return 1;
    }

    HANDLE threads[2] = { threadComandos, threadAlertas };
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);

    CloseHandle(threadComandos);
    CloseHandle(threadAlertas);
    CloseHandle(contexto.eventoNotificar);
    CloseHandle(contexto.eventoParar);
    DeleteCriticalSection(&contexto.csEscrita);
    return 0;
}
