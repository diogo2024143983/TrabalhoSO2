#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include "../protocolo.h"

#define CHAVE_REGISTO   _T("Software\\TrabSO2")
#define VALOR_NPIPE     _T("NPIPE")
#define TAM_MAX_COMANDO 256
#define TAM_NOME_PIPE   256
#define PREFIXO_PIPE    _T("\\\\.\\pipe\\")

typedef struct {
    TCHAR  nomePipe[TAM_NOME_PIPE];
    HANDLE hPipe;
    DWORD  identificador;
    volatile LONG ligado;
    volatile LONG deveSair;
    volatile LONG temAlerta;
    MSG_ALERTA alertaAtivo;
    CRITICAL_SECTION csConsola;
    CRITICAL_SECTION csPipe;
    CRITICAL_SECTION csAlerta;
    HANDLE eventoParar;
    HANDLE eventoNovoAlerta;
    HANDLE eventoCancelarAlerta;
    HANDLE eventoRespostaLigar;
    HANDLE eventoRespostaDesligar;
    volatile LONG esperandoInput;
} CONTEXTO_APP;

static void PrintConsola(CONTEXTO_APP* ctx, const TCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EnterCriticalSection(&ctx->csConsola);
    _tprintf(_T("\r%*s\r"), 120, _T(""));
    _vtprintf(fmt, args);
    if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0 &&
        InterlockedCompareExchange(&ctx->esperandoInput, 0, 0) != 0) {
        _tprintf(_T("CMD> "));
    }
    LeaveCriticalSection(&ctx->csConsola);
    va_end(args);
}

static void ImprimirComTimestamp(CONTEXTO_APP* ctx, const TCHAR* texto) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&ctx->csConsola);
    _tprintf(_T("\r%*s\r"), 120, _T(""));
    _tprintf(_T("%02u/%02u/%04u (%02u:%02u:%02u): '%s'\n"),
        st.wDay, st.wMonth, st.wYear,
        st.wHour, st.wMinute, st.wSecond,
        texto);
    if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0 &&
        InterlockedCompareExchange(&ctx->esperandoInput, 0, 0) != 0) {
        _tprintf(_T("CMD> "));
    }
    LeaveCriticalSection(&ctx->csConsola);
}

static BOOL GarantirNomePipeNoRegisto(const TCHAR* daLinhaComandos, TCHAR* nomePipeSaida, DWORD tamanhoSaida) {
    HKEY chave = NULL;
    LONG res;
    DWORD tipo = 0;
    DWORD bytesDados = tamanhoSaida * sizeof(TCHAR);

    res = RegCreateKeyEx(HKEY_CURRENT_USER, CHAVE_REGISTO, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &chave, NULL);
    if (res != ERROR_SUCCESS) return FALSE;

    if (daLinhaComandos != NULL && daLinhaComandos[0] != _T('\0')) {
        _tcsncpy_s(nomePipeSaida, tamanhoSaida, daLinhaComandos, _TRUNCATE);
        res = RegSetValueEx(chave, VALOR_NPIPE, 0, REG_SZ, (const BYTE*)nomePipeSaida, (DWORD)((_tcslen(nomePipeSaida) + 1) * sizeof(TCHAR)));
        RegCloseKey(chave);
        return res == ERROR_SUCCESS;
    }

    res = RegQueryValueEx(chave, VALOR_NPIPE, NULL, &tipo, (LPBYTE)nomePipeSaida, &bytesDados);
    RegCloseKey(chave);

    if (res != ERROR_SUCCESS || tipo != REG_SZ || nomePipeSaida[0] == _T('\0')) return FALSE;
    return TRUE;
}

static BOOL EscreverPipe(CONTEXTO_APP* ctx, const void* dados, DWORD tam) {
    DWORD escritos = 0;
    EnterCriticalSection(&ctx->csPipe);
    BOOL ok = WriteFile(ctx->hPipe, dados, tam, &escritos, NULL);
    LeaveCriticalSection(&ctx->csPipe);
    return ok && escritos == tam;
}

static DWORD WINAPI ThreadAlertas(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    HANDLE espNovoAlerta[2];
    HANDLE espDuracao[4];
    DWORD resDuracao;
    MSG_ALERTA alerta;
    HANDLE timer;
    LARGE_INTEGER li;
    MSG_CMD cmd3;

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (timer == NULL) return 1;

    espNovoAlerta[0] = ctx->eventoParar;
    espNovoAlerta[1] = ctx->eventoNovoAlerta;

    for (;;) {
        DWORD res = WaitForMultipleObjects(2, espNovoAlerta, FALSE, INFINITE);
        if (res == WAIT_OBJECT_0) break;
        if (res != WAIT_OBJECT_0 + 1) continue;

        EnterCriticalSection(&ctx->csAlerta);
        alerta = ctx->alertaAtivo;
        LeaveCriticalSection(&ctx->csAlerta);

        alerta.msg[_countof(alerta.msg) - 1] = _T('\0');
        ImprimirComTimestamp(ctx, alerta.msg);

        li.QuadPart = -((LONGLONG)alerta.duracao * 10000000LL);
        if (!SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE)) continue;

        espDuracao[0] = ctx->eventoParar;
        espDuracao[1] = ctx->eventoNovoAlerta;
        espDuracao[2] = ctx->eventoCancelarAlerta;
        espDuracao[3] = timer;

        for (;;) {
            resDuracao = WaitForMultipleObjects(4, espDuracao, FALSE, INFINITE);
            if (resDuracao == WAIT_OBJECT_0) {
                CloseHandle(timer);
                return 0;
            }
            else if (resDuracao == WAIT_OBJECT_0 + 1) {
                EnterCriticalSection(&ctx->csAlerta);
                alerta = ctx->alertaAtivo;
                LeaveCriticalSection(&ctx->csAlerta);
                alerta.msg[_countof(alerta.msg) - 1] = _T('\0');
                ImprimirComTimestamp(ctx, alerta.msg);
                li.QuadPart = -((LONGLONG)alerta.duracao * 10000000LL);
                SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
            }
            else if (resDuracao == WAIT_OBJECT_0 + 2) {
                CancelWaitableTimer(timer);
                ImprimirComTimestamp(ctx, _T("---"));
                break;
            }
            else if (resDuracao == WAIT_OBJECT_0 + 3) {
                ImprimirComTimestamp(ctx, _T("---"));
                cmd3.tipo = TIPO_FIM_ALERTA;
                EscreverPipe(ctx, &cmd3, sizeof(MSG_CMD));
                EnterCriticalSection(&ctx->csAlerta);
                ctx->temAlerta = 0;
                LeaveCriticalSection(&ctx->csAlerta);
                break;
            }
        }
    }
    CloseHandle(timer);
    return 0;
}

static DWORD WINAPI ThreadReceberCentral(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    BYTE buf[sizeof(MSG_ALERTA) + 32];
    DWORD lidos;
    BOOL ok;
    BYTE tipo;
    MSG_ALERTA* alerta;
    MSG_ID* mid;
    MSG_CMD conf;

    for (;;) {
        if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;

        lidos = 0;
        ok = ReadFile(ctx->hPipe, buf, sizeof(buf), &lidos, NULL);

        if (!ok || lidos == 0) {
            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
                InterlockedExchange(&ctx->deveSair, 1);
                SetEvent(ctx->eventoParar);
                SetEvent(ctx->eventoRespostaLigar);
                SetEvent(ctx->eventoRespostaDesligar);
            }
            break;
        }

        tipo = buf[0];

        if (tipo == TIPO_NOVO_ALERTA) {
            if (lidos < sizeof(MSG_ALERTA)) continue;
            alerta = (MSG_ALERTA*)buf;
            alerta->msg[_countof(alerta->msg) - 1] = _T('\0');

            EnterCriticalSection(&ctx->csAlerta);
            ctx->alertaAtivo = *alerta;
            ctx->temAlerta = 1;
            LeaveCriticalSection(&ctx->csAlerta);

            EscreverPipe(ctx, alerta, sizeof(MSG_ALERTA));
            SetEvent(ctx->eventoNovoAlerta);

        }
        else if (tipo == TIPO_CANCELAR) {
            conf.tipo = TIPO_CANCELAR;
            EscreverPipe(ctx, &conf, sizeof(MSG_CMD));

            EnterCriticalSection(&ctx->csAlerta);
            ctx->temAlerta = 0;
            LeaveCriticalSection(&ctx->csAlerta);

            SetEvent(ctx->eventoCancelarAlerta);

        }
        else if (tipo == TIPO_ENCERRAR) {
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            SetEvent(ctx->eventoRespostaLigar);
            SetEvent(ctx->eventoRespostaDesligar);
            ExitProcess(0);

        }
        else if (tipo == TIPO_RESPOSTA_ID) {
            if (lidos >= sizeof(MSG_ID)) {
                mid = (MSG_ID*)buf;
                ctx->identificador = mid->identificador;
            }
            SetEvent(ctx->eventoRespostaLigar);

        }
        else if (tipo == TIPO_DESLIGAR) {
            SetEvent(ctx->eventoRespostaDesligar);
        }
    }
    return 0;
}

static DWORD WINAPI ThreadComandos(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    TCHAR comando[TAM_MAX_COMANDO];
    size_t tam;
    MSG_CMD cmd;
    HANDLE espLigar[2];
    HANDLE espDesligar[2];
    DWORD res;

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        EnterCriticalSection(&ctx->csConsola);
        _tprintf(_T("CMD> "));
        LeaveCriticalSection(&ctx->csConsola);
        InterlockedExchange(&ctx->esperandoInput, 1);

        if (_fgetts(comando, TAM_MAX_COMANDO, stdin) == NULL) {
            InterlockedExchange(&ctx->esperandoInput, 0);
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            break;
        }
        InterlockedExchange(&ctx->esperandoInput, 0);

        tam = _tcslen(comando);
        while (tam > 0 && (comando[tam - 1] == _T('\n') || comando[tam - 1] == _T('\r')))
            comando[--tam] = _T('\0');

        if (tam == 0) continue;

        if (_tcsicmp(comando, _T("ligar")) == 0) {
            if (InterlockedCompareExchange(&ctx->ligado, 0, 0) != 0) continue;

            cmd.tipo = TIPO_LIGAR;
            if (!EscreverPipe(ctx, &cmd, sizeof(MSG_CMD))) continue;

            espLigar[0] = ctx->eventoParar;
            espLigar[1] = ctx->eventoRespostaLigar;
            res = WaitForMultipleObjects(2, espLigar, FALSE, 5000);

            if (res != WAIT_OBJECT_0 + 1) continue;

            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;

            InterlockedExchange(&ctx->ligado, 1);
            PrintConsola(ctx, _T("Identificador = %lu\n"), ctx->identificador);

        }
        else if (_tcsicmp(comando, _T("desligar")) == 0) {
            if (InterlockedCompareExchange(&ctx->ligado, 0, 0) == 0) {
                InterlockedExchange(&ctx->deveSair, 1);
                SetEvent(ctx->eventoParar);
                break;
            }

            cmd.tipo = TIPO_DESLIGAR;
            EscreverPipe(ctx, &cmd, sizeof(MSG_CMD));

            espDesligar[0] = ctx->eventoParar;
            espDesligar[1] = ctx->eventoRespostaDesligar;
            WaitForMultipleObjects(2, espDesligar, FALSE, 5000);

            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            break;
        }
    }
    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
    CONTEXTO_APP ctx;
    TCHAR nomePipeCompleto[TAM_NOME_PIPE + 16];
    BOOL ligadoPipe;
    int tentativa;
    DWORD err;
    DWORD modoLeitura;
    HANDLE hThreadCmds, hThreadAlertas, hThreadCentral;
    HANDLE threads[3];
    const TCHAR* argPipe;

    ZeroMemory(&ctx, sizeof(ctx));
    InitializeCriticalSection(&ctx.csConsola);
    InitializeCriticalSection(&ctx.csPipe);
    InitializeCriticalSection(&ctx.csAlerta);

    argPipe = (argc >= 2 && argv[1] != NULL && argv[1][0] != _T('\0')) ? argv[1] : NULL;

    if (!GarantirNomePipeNoRegisto(argPipe, ctx.nomePipe, _countof(ctx.nomePipe))) {
        DeleteCriticalSection(&ctx.csConsola);
        DeleteCriticalSection(&ctx.csPipe);
        DeleteCriticalSection(&ctx.csAlerta);
        return 1;
    }

    _sntprintf(nomePipeCompleto, _countof(nomePipeCompleto), _T("%s%s"), PREFIXO_PIPE, ctx.nomePipe);

    ligadoPipe = FALSE;
    for (tentativa = 0; tentativa < 5; tentativa++) {
        ctx.hPipe = CreateFile(nomePipeCompleto, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (ctx.hPipe != INVALID_HANDLE_VALUE) {
            ligadoPipe = TRUE;
            break;
        }
        err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipe(nomePipeCompleto, 2000);
        }
        else {
            Sleep(1000);
        }
    }

    if (!ligadoPipe) {
        DeleteCriticalSection(&ctx.csConsola);
        DeleteCriticalSection(&ctx.csPipe);
        DeleteCriticalSection(&ctx.csAlerta);
        return 1;
    }

    modoLeitura = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(ctx.hPipe, &modoLeitura, NULL, NULL);

    ctx.eventoParar = CreateEvent(NULL, TRUE, FALSE, NULL);
    ctx.eventoNovoAlerta = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx.eventoCancelarAlerta = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx.eventoRespostaLigar = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx.eventoRespostaDesligar = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!ctx.eventoParar || !ctx.eventoNovoAlerta || !ctx.eventoCancelarAlerta || !ctx.eventoRespostaLigar || !ctx.eventoRespostaDesligar) return 1;

    hThreadCmds = CreateThread(NULL, 0, ThreadComandos, &ctx, 0, NULL);
    hThreadAlertas = CreateThread(NULL, 0, ThreadAlertas, &ctx, 0, NULL);
    hThreadCentral = CreateThread(NULL, 0, ThreadReceberCentral, &ctx, 0, NULL);

    if (!hThreadCmds || !hThreadAlertas || !hThreadCentral) return 1;

    threads[0] = hThreadCmds;
    threads[1] = hThreadAlertas;
    threads[2] = hThreadCentral;
    WaitForMultipleObjects(3, threads, TRUE, INFINITE);

    CloseHandle(hThreadCmds);
    CloseHandle(hThreadAlertas);
    CloseHandle(hThreadCentral);
    CloseHandle(ctx.eventoParar);
    CloseHandle(ctx.eventoNovoAlerta);
    CloseHandle(ctx.eventoCancelarAlerta);
    CloseHandle(ctx.eventoRespostaLigar);
    CloseHandle(ctx.eventoRespostaDesligar);
    CloseHandle(ctx.hPipe);
    DeleteCriticalSection(&ctx.csConsola);
    DeleteCriticalSection(&ctx.csPipe);
    DeleteCriticalSection(&ctx.csAlerta);

    return 0;
}