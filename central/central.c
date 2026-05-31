#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "../protocolo.h"

#define TAM_MAX_COMANDO 512
#define TAM_NOME_PIPE   256

HANDLE hMapFileSHM;
SHM_ALERTA* pDadosSHM;
HANDLE hMutexSHM;
HANDLE hEventSHM;

typedef struct {
    BOOL   ativo;
    DWORD  identificador;
    HANDLE hPipe;
    BOOL   temAlerta;
    TCHAR  msgAlerta[TAM_MSG];
    DWORD  duracaoInicial;
    DWORD  tickInicio;
    CRITICAL_SECTION csPipe;
    HANDLE eventoConfirmacao;
} ESTADO_PLACAR;

typedef struct {
    TCHAR nomePipe[TAM_NOME_PIPE];
    ESTADO_PLACAR placares[MAX_PLACAR];
    DWORD proximoId;
    CRITICAL_SECTION csPlacares;
    volatile LONG deveSair;
    HANDLE eventoParar;
    CRITICAL_SECTION csConsola;
} CONTEXTO_APP;

static void PrintConsola(CONTEXTO_APP* ctx, const TCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EnterCriticalSection(&ctx->csConsola);
    _vtprintf(fmt, args);
    LeaveCriticalSection(&ctx->csConsola);
    va_end(args);
}

void AtualizarSHM(CONTEXTO_APP* ctx) {
    int idx_shm = 0;
    DWORD agora = GetTickCount();
    WaitForSingleObject(hMutexSHM, INFINITE);
    if (pDadosSHM) {
        for (int i = 0; i < MAX_PLACAR; i++) {
            if (ctx->placares[i].ativo && ctx->placares[i].temAlerta) {
                pDadosSHM->alertas[idx_shm].identificador = ctx->placares[i].identificador;

                DWORD decorrido = (agora - ctx->placares[i].tickInicio) / 1000;
                if (ctx->placares[i].duracaoInicial > decorrido) {
                    pDadosSHM->alertas[idx_shm].duracao = ctx->placares[i].duracaoInicial - decorrido;
                }
                else {
                    pDadosSHM->alertas[idx_shm].duracao = 0;
                }

                _tcsncpy_s(pDadosSHM->alertas[idx_shm].msg, 140, ctx->placares[i].msgAlerta, _TRUNCATE);
                idx_shm++;
            }
        }
        pDadosSHM->num_alertas = idx_shm;
    }
    ReleaseMutex(hMutexSHM);
    SetEvent(hEventSHM);
}

static DWORD WINAPI ThreadTimerSHM(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        AtualizarSHM(ctx);
        Sleep(1000);
    }
    return 0;
}

static int EncontrarPlacarPorId(CONTEXTO_APP* ctx, DWORD id) {
    int i;
    for (i = 0; i < MAX_PLACAR; i++) {
        if (ctx->placares[i].ativo && ctx->placares[i].identificador == id)
            return i;
    }
    return -1;
}

static int EncontrarSlotLivre(CONTEXTO_APP* ctx) {
    int i;
    for (i = 0; i < MAX_PLACAR; i++) {
        if (!ctx->placares[i].ativo)
            return i;
    }
    return -1;
}

static BOOL EscreverPipePlacar(CONTEXTO_APP* ctx, int idx, const void* dados, DWORD tam) {
    DWORD escritos = 0;
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    EnterCriticalSection(&ctx->placares[idx].csPipe);
    BOOL ok = WriteFile(ctx->placares[idx].hPipe, dados, tam, &escritos, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, INFINITE);
        ok = GetOverlappedResult(ctx->placares[idx].hPipe, &ov, &escritos, FALSE);
    }
    LeaveCriticalSection(&ctx->placares[idx].csPipe);

    CloseHandle(ov.hEvent);
    return ok && (escritos == tam);
}

static BOOL ParseDWORDStrict(const TCHAR* texto, DWORD* valorOut) {
    TCHAR* fim = NULL;
    unsigned long v;

    if (texto == NULL || texto[0] == _T('\0') || valorOut == NULL) return FALSE;

    errno = 0;
    v = _tcstoul(texto, &fim, 10);
    if (fim == texto || *fim != _T('\0') || errno == ERANGE || v > 0xFFFFFFFFUL) {
        return FALSE;
    }

    *valorOut = (DWORD)v;
    return TRUE;
}

typedef struct {
    CONTEXTO_APP* ctx;
    int           idx;
} ARGS_THREAD_PLACAR;

static DWORD WINAPI ThreadPlacar(LPVOID param) {
    ARGS_THREAD_PLACAR* args = (ARGS_THREAD_PLACAR*)param;
    CONTEXTO_APP* ctx = args->ctx;
    int idx = args->idx;
    free(args);

    HANDLE hPipe = ctx->placares[idx].hPipe;
    BYTE buf[sizeof(MSG_ALERTA) + 32];
    DWORD lidos;
    BOOL ok;
    BYTE tipo;
    DWORD id;

    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    for (;;) {
        lidos = 0;
        ok = ReadFile(hPipe, buf, sizeof(buf), &lidos, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ctx->eventoParar, ov.hEvent };
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) { CancelIo(hPipe); break; }
            ok = GetOverlappedResult(hPipe, &ov, &lidos, FALSE);
        }

        if (!ok || lidos == 0) {
            PrintConsola(ctx, _T("[Central] Placar %lu desligou-se.\n"), ctx->placares[idx].identificador);
            break;
        }

        tipo = buf[0];

        if (tipo == TIPO_LIGAR) {
            EnterCriticalSection(&ctx->csPlacares);
            id = ctx->proximoId++;
            ctx->placares[idx].identificador = id;
            LeaveCriticalSection(&ctx->csPlacares);

            MSG_ID mid;
            mid.tipo = TIPO_RESPOSTA_ID;
            mid.identificador = id;
            EscreverPipePlacar(ctx, idx, &mid, sizeof(MSG_ID));
            PrintConsola(ctx, _T("[Central] Placar ligado com identificador %lu.\n"), id);

        }
        else if (tipo == TIPO_DESLIGAR) {
            MSG_CMD conf;
            conf.tipo = TIPO_DESLIGAR;
            EscreverPipePlacar(ctx, idx, &conf, sizeof(MSG_CMD));
            PrintConsola(ctx, _T("[Central] Placar %lu desligou-se.\n"), ctx->placares[idx].identificador);
            break;

        }
        else if (tipo == TIPO_FIM_ALERTA) {
            EnterCriticalSection(&ctx->csPlacares);
            ctx->placares[idx].temAlerta = FALSE;
            ctx->placares[idx].msgAlerta[0] = _T('\0');
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Alerta do placar %lu expirou.\n"), ctx->placares[idx].identificador);
            AtualizarSHM(ctx);
        }
        else if (tipo == TIPO_NOVO_ALERTA) {
            SetEvent(ctx->placares[idx].eventoConfirmacao);

        }
        else if (tipo == TIPO_CANCELAR) {
            SetEvent(ctx->placares[idx].eventoConfirmacao);

        }
        else {
            PrintConsola(ctx, _T("[Central] Mensagem desconhecida do placar %lu.\n"), ctx->placares[idx].identificador);
        }
    }

    EnterCriticalSection(&ctx->csPlacares);
    CloseHandle(hPipe);
    ctx->placares[idx].ativo = FALSE;
    ctx->placares[idx].hPipe = NULL;
    ctx->placares[idx].identificador = 0;
    ctx->placares[idx].temAlerta = FALSE;
    ctx->placares[idx].msgAlerta[0] = _T('\0');
    LeaveCriticalSection(&ctx->csPlacares);

    AtualizarSHM(ctx);
    CloseHandle(ov.hEvent);

    return 0;
}

static DWORD WINAPI ThreadAceitarLigacoes(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    TCHAR nomePipeCompleto[TAM_NOME_PIPE + 10];
    HANDLE hPipe;
    BOOL ligado;
    int slot;
    ARGS_THREAD_PLACAR* args;
    HANDLE hThread;

    _sntprintf(nomePipeCompleto, _countof(nomePipeCompleto), _T("\\\\.\\pipe\\%s"), ctx->nomePipe);

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        hPipe = CreateNamedPipe(
            nomePipeCompleto,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            MAX_PLACAR, 4096, 4096, 0, NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

        OVERLAPPED ovConn = { 0 };
        ovConn.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        ligado = ConnectNamedPipe(hPipe, &ovConn);

        if (!ligado) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = { ctx->eventoParar, ovConn.hEvent };
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    CancelIo(hPipe);
                    CloseHandle(hPipe);
                    CloseHandle(ovConn.hEvent);
                    break;
                }
                DWORD dummy;
                ligado = GetOverlappedResult(hPipe, &ovConn, &dummy, FALSE);
            }
            else if (err == ERROR_PIPE_CONNECTED) {
                ligado = TRUE;
            }
        }
        CloseHandle(ovConn.hEvent);

        if (!ligado) { CloseHandle(hPipe); continue; }

        EnterCriticalSection(&ctx->csPlacares);
        slot = EncontrarSlotLivre(ctx);
        if (slot < 0) {
            LeaveCriticalSection(&ctx->csPlacares);
            CloseHandle(hPipe);
            continue;
        }
        ctx->placares[slot].ativo = TRUE;
        ctx->placares[slot].hPipe = hPipe;
        ctx->placares[slot].identificador = 0;
        ctx->placares[slot].temAlerta = FALSE;
        ctx->placares[slot].msgAlerta[0] = _T('\0');
        ResetEvent(ctx->placares[slot].eventoConfirmacao);
        LeaveCriticalSection(&ctx->csPlacares);

        args = (ARGS_THREAD_PLACAR*)malloc(sizeof(ARGS_THREAD_PLACAR));
        args->ctx = ctx;
        args->idx = slot;

        hThread = CreateThread(NULL, 0, ThreadPlacar, args, 0, NULL);
        CloseHandle(hThread);
    }
    return 0;
}

static void CmdAlerta(CONTEXTO_APP* ctx, TCHAR* args) {
    TCHAR copia[TAM_MAX_COMANDO];
    TCHAR* tokens[512];
    TCHAR* ctx_tok;
    TCHAR* t;
    int n, i;
    DWORD duracao, idPlacar;
    TCHAR msg[TAM_MSG];
    MSG_ALERTA alerta;
    int destinos[MAX_PLACAR];
    HANDLE eventos[MAX_PLACAR];
    int nDestinos = 0;

    if (args == NULL || args[0] == _T('\0')) return;

    _tcsncpy_s(copia, _countof(copia), args, _TRUNCATE);
    ctx_tok = NULL;
    n = 0;
    t = _tcstok_s(copia, _T(" \t"), &ctx_tok);
    while (t && n < 512) { tokens[n++] = t; t = _tcstok_s(NULL, _T(" \t"), &ctx_tok); }

    if (n < 3) return;

    if (!ParseDWORDStrict(tokens[n - 2], &duracao) || !ParseDWORDStrict(tokens[n - 1], &idPlacar)) return;

    msg[0] = _T('\0');
    for (i = 0; i < n - 2; i++) {
        if (i > 0) _tcsncat_s(msg, _countof(msg), _T(" "), _TRUNCATE);
        _tcsncat_s(msg, _countof(msg), tokens[i], _TRUNCATE);
    }

    alerta.tipo = TIPO_NOVO_ALERTA;
    _tcsncpy_s(alerta.msg, _countof(alerta.msg), msg, _TRUNCATE);
    alerta.duracao = duracao;

    EnterCriticalSection(&ctx->csPlacares);

    if (idPlacar == 0) {
        for (i = 0; i < MAX_PLACAR; i++) {
            if (!ctx->placares[i].ativo) continue;
            if (EscreverPipePlacar(ctx, i, &alerta, sizeof(MSG_ALERTA))) {
                destinos[nDestinos] = i;
                eventos[nDestinos] = ctx->placares[i].eventoConfirmacao;
                nDestinos++;
            }
        }
        LeaveCriticalSection(&ctx->csPlacares);

        for (i = 0; i < nDestinos; i++) {
            WaitForSingleObject(eventos[i], 5000);
            EnterCriticalSection(&ctx->csPlacares);
            if (ctx->placares[destinos[i]].ativo) {
                ctx->placares[destinos[i]].temAlerta = TRUE;
                _tcsncpy_s(ctx->placares[destinos[i]].msgAlerta, TAM_MSG, msg, _TRUNCATE);
                ctx->placares[destinos[i]].duracaoInicial = duracao;
                ctx->placares[destinos[i]].tickInicio = GetTickCount();
            }
            LeaveCriticalSection(&ctx->csPlacares);
        }
        AtualizarSHM(ctx);
    }
    else {
        int idx = EncontrarPlacarPorId(ctx, idPlacar);
        if (idx < 0) {
            LeaveCriticalSection(&ctx->csPlacares);
            return;
        }
        HANDLE evConf = ctx->placares[idx].eventoConfirmacao;
        if (EscreverPipePlacar(ctx, idx, &alerta, sizeof(MSG_ALERTA))) {
            LeaveCriticalSection(&ctx->csPlacares);
            WaitForSingleObject(evConf, 5000);
            EnterCriticalSection(&ctx->csPlacares);
            if (ctx->placares[idx].ativo) {
                ctx->placares[idx].temAlerta = TRUE;
                _tcsncpy_s(ctx->placares[idx].msgAlerta, TAM_MSG, msg, _TRUNCATE);
                ctx->placares[idx].duracaoInicial = duracao;
                ctx->placares[idx].tickInicio = GetTickCount();
            }
            LeaveCriticalSection(&ctx->csPlacares);
            AtualizarSHM(ctx);
        }
        else {
            LeaveCriticalSection(&ctx->csPlacares);
        }
    }
}

static void CmdCancelar(CONTEXTO_APP* ctx, TCHAR* args) {
    DWORD idPlacar;
    int idx;
    HANDLE evConf;
    MSG_CMD cmd;

    if (args == NULL || args[0] == _T('\0')) return;
    if (!ParseDWORDStrict(args, &idPlacar)) return;

    EnterCriticalSection(&ctx->csPlacares);
    idx = EncontrarPlacarPorId(ctx, idPlacar);
    if (idx < 0 || !ctx->placares[idx].temAlerta) {
        LeaveCriticalSection(&ctx->csPlacares);
        return;
    }

    evConf = ctx->placares[idx].eventoConfirmacao;
    cmd.tipo = TIPO_CANCELAR;
    if (EscreverPipePlacar(ctx, idx, &cmd, sizeof(MSG_CMD))) {
        LeaveCriticalSection(&ctx->csPlacares);
        WaitForSingleObject(evConf, 5000);
        EnterCriticalSection(&ctx->csPlacares);
        if (ctx->placares[idx].ativo) {
            ctx->placares[idx].temAlerta = FALSE;
            ctx->placares[idx].msgAlerta[0] = _T('\0');
        }
        LeaveCriticalSection(&ctx->csPlacares);
        AtualizarSHM(ctx);
    }
    else {
        LeaveCriticalSection(&ctx->csPlacares);
    }
}

static void CmdListar(CONTEXTO_APP* ctx) {
    int i, encontrou;
    EnterCriticalSection(&ctx->csPlacares);
    PrintConsola(ctx, _T("--- Lista de placares ---\n"));
    encontrou = 0;
    for (i = 0; i < MAX_PLACAR; i++) {
        if (!ctx->placares[i].ativo) continue;
        encontrou = 1;
        if (ctx->placares[i].temAlerta) {
            PrintConsola(ctx, _T("  Placar %lu: alerta ativo = '%s'\n"), ctx->placares[i].identificador, ctx->placares[i].msgAlerta);
        }
        else {
            PrintConsola(ctx, _T("  Placar %lu: sem alerta ativo\n"), ctx->placares[i].identificador);
        }
    }
    if (!encontrou) PrintConsola(ctx, _T("  (nenhum placar ligado)\n"));
    PrintConsola(ctx, _T("-------------------------\n"));
    LeaveCriticalSection(&ctx->csPlacares);
}

static void CmdEncerrar(CONTEXTO_APP* ctx) {
    int i;
    MSG_CMD cmd;
    cmd.tipo = TIPO_ENCERRAR;

    InterlockedExchange(&ctx->deveSair, 1);

    EnterCriticalSection(&ctx->csPlacares);
    for (i = 0; i < MAX_PLACAR; i++) {
        if (!ctx->placares[i].ativo) continue;
        EscreverPipePlacar(ctx, i, &cmd, sizeof(MSG_CMD));
    }
    LeaveCriticalSection(&ctx->csPlacares);

    WaitForSingleObject(hMutexSHM, INFINITE);
    if (pDadosSHM) pDadosSHM->desligar = TRUE;
    ReleaseMutex(hMutexSHM);
    SetEvent(hEventSHM);

    SetEvent(ctx->eventoParar);
}

static DWORD WINAPI ThreadComandos(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    TCHAR linha[TAM_MAX_COMANDO];
    size_t len;
    TCHAR* ctx_tok;
    TCHAR* cmd;
    TCHAR* restArgs;

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        EnterCriticalSection(&ctx->csConsola);
        _tprintf(_T("CMD> "));
        LeaveCriticalSection(&ctx->csConsola);

        if (_fgetts(linha, _countof(linha), stdin) == NULL) {
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            break;
        }

        len = _tcslen(linha);
        while (len > 0 && (linha[len - 1] == _T('\n') || linha[len - 1] == _T('\r')))
            linha[--len] = _T('\0');

        if (len == 0) continue;

        ctx_tok = NULL;
        cmd = _tcstok_s(linha, _T(" \t"), &ctx_tok);
        if (cmd == NULL) continue;
        restArgs = ctx_tok;

        if (_tcsicmp(cmd, _T("alerta")) == 0) {
            CmdAlerta(ctx, restArgs);
        }
        else if (_tcsicmp(cmd, _T("cancelar")) == 0) {
            CmdCancelar(ctx, restArgs);
        }
        else if (_tcsicmp(cmd, _T("listar")) == 0) {
            CmdListar(ctx);
        }
        else if (_tcsicmp(cmd, _T("encerrar")) == 0) {
            CmdEncerrar(ctx);
            break;
        }
    }
    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
    CONTEXTO_APP ctx;
    HANDLE hThreadLigacoes, hThreadCmds, hThreadTimerSHM;
    int i;

    if (argc < 2 || argv[1] == NULL || argv[1][0] == _T('\0')) return 1;

    hMapFileSHM = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SHM_ALERTA), _T("Local\\SO2_SHM"));
    if (hMapFileSHM) pDadosSHM = (SHM_ALERTA*)MapViewOfFile(hMapFileSHM, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_ALERTA));
    hMutexSHM = CreateMutex(NULL, FALSE, _T("Local\\SO2_MUTEX"));
    hEventSHM = CreateEvent(NULL, TRUE, FALSE, _T("Local\\SO2_EVENT"));
    if (pDadosSHM) pDadosSHM->desligar = FALSE;

    ZeroMemory(&ctx, sizeof(ctx));
    _tcsncpy_s(ctx.nomePipe, _countof(ctx.nomePipe), argv[1], _TRUNCATE);
    ctx.proximoId = 1;

    InitializeCriticalSection(&ctx.csPlacares);
    InitializeCriticalSection(&ctx.csConsola);
    for (i = 0; i < MAX_PLACAR; i++) {
        InitializeCriticalSection(&ctx.placares[i].csPipe);
        ctx.placares[i].eventoConfirmacao = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (ctx.placares[i].eventoConfirmacao == NULL) {
            while (--i >= 0) {
                CloseHandle(ctx.placares[i].eventoConfirmacao);
                DeleteCriticalSection(&ctx.placares[i].csPipe);
            }
            DeleteCriticalSection(&ctx.csPlacares);
            DeleteCriticalSection(&ctx.csConsola);
            return 1;
        }
    }

    ctx.eventoParar = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ctx.eventoParar == NULL) return 1;

    hThreadLigacoes = CreateThread(NULL, 0, ThreadAceitarLigacoes, &ctx, 0, NULL);
    if (hThreadLigacoes == NULL) return 1;

    hThreadTimerSHM = CreateThread(NULL, 0, ThreadTimerSHM, &ctx, 0, NULL);
    if (hThreadTimerSHM == NULL) return 1;

    hThreadCmds = CreateThread(NULL, 0, ThreadComandos, &ctx, 0, NULL);
    if (hThreadCmds == NULL) {
        InterlockedExchange(&ctx.deveSair, 1);
        SetEvent(ctx.eventoParar);
        WaitForSingleObject(hThreadLigacoes, 3000);
        return 1;
    }

    WaitForSingleObject(hThreadCmds, INFINITE);

    InterlockedExchange(&ctx.deveSair, 1);
    SetEvent(ctx.eventoParar);
    WaitForSingleObject(hThreadLigacoes, 3000);
    WaitForSingleObject(hThreadTimerSHM, 3000);

    CloseHandle(hThreadCmds);
    CloseHandle(hThreadLigacoes);
    CloseHandle(hThreadTimerSHM);
    CloseHandle(ctx.eventoParar);

    EnterCriticalSection(&ctx.csPlacares);
    for (i = 0; i < MAX_PLACAR; i++) {
        if (ctx.placares[i].ativo && ctx.placares[i].hPipe != NULL) {
            CloseHandle(ctx.placares[i].hPipe);
            ctx.placares[i].hPipe = NULL;
        }
        if (ctx.placares[i].eventoConfirmacao)
            CloseHandle(ctx.placares[i].eventoConfirmacao);
        DeleteCriticalSection(&ctx.placares[i].csPipe);
    }
    LeaveCriticalSection(&ctx.csPlacares);

    DeleteCriticalSection(&ctx.csPlacares);
    DeleteCriticalSection(&ctx.csConsola);

    if (pDadosSHM) UnmapViewOfFile(pDadosSHM);
    if (hMapFileSHM) CloseHandle(hMapFileSHM);
    if (hMutexSHM) CloseHandle(hMutexSHM);
    if (hEventSHM) CloseHandle(hEventSHM);

    return 0;
}