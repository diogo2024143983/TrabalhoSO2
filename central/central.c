#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define MAX_PLACARES    20
#define TAM_MAX_COMANDO 512
#define TAM_NOME_PIPE   256

/* ------------------------------------------------------------------ */
/* Estruturas de mensagem (conforme enunciado)                         */
/* ------------------------------------------------------------------ */
typedef struct {
    BYTE tipo;          /* 1-ligar, 2-desligar, 3-fim alerta */
} MSG_CMD;

typedef struct {
    BYTE  tipo;         /* 4 - novo alerta */
    TCHAR msg[140];
    DWORD duracao;
} MSG_ALERTA;

typedef struct {
    BYTE  tipo;         /* 7 - atribuir identificador */
    DWORD identificador;
} MSG_ID;

/* ------------------------------------------------------------------ */
/* Estado de cada placar                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    BOOL   ativo;
    DWORD  identificador;
    HANDLE hPipe;
    BOOL   temAlerta;
    TCHAR  msgAlerta[140];
    CRITICAL_SECTION csPipe;    /* protege escritas no pipe */
    HANDLE eventoConfirmacao;   /* auto-reset: sinalizado quando chega confirmacao */
} ESTADO_PLACAR;

/* ------------------------------------------------------------------ */
/* Contexto global da aplicacao                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    TCHAR nomePipe[TAM_NOME_PIPE];

    ESTADO_PLACAR placares[MAX_PLACARES];
    DWORD proximoId;
    CRITICAL_SECTION csPlacares;

    volatile LONG deveSair;
    HANDLE eventoParar;

    CRITICAL_SECTION csConsola;
} CONTEXTO_APP;

/* ------------------------------------------------------------------ */
/* Utilitarios                                                         */
/* ------------------------------------------------------------------ */
static void PrintConsola(CONTEXTO_APP* ctx, const TCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EnterCriticalSection(&ctx->csConsola);
    _vtprintf(fmt, args);
    LeaveCriticalSection(&ctx->csConsola);
    va_end(args);
}

static int EncontrarPlacarPorId(CONTEXTO_APP* ctx, DWORD id) {
    int i;
    for (i = 0; i < MAX_PLACARES; i++) {
        if (ctx->placares[i].ativo && ctx->placares[i].identificador == id)
            return i;
    }
    return -1;
}

static int EncontrarSlotLivre(CONTEXTO_APP* ctx) {
    int i;
    for (i = 0; i < MAX_PLACARES; i++) {
        if (!ctx->placares[i].ativo)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Escrita no pipe de um placar (protegida por csPipe individual)      */
/* ------------------------------------------------------------------ */
static BOOL EscreverPipePlacar(CONTEXTO_APP* ctx, int idx, const void* dados, DWORD tam) {
    DWORD escritos = 0;
    EnterCriticalSection(&ctx->placares[idx].csPipe);
    BOOL ok = WriteFile(ctx->placares[idx].hPipe, dados, tam, &escritos, NULL);
    LeaveCriticalSection(&ctx->placares[idx].csPipe);
    return ok && escritos == tam;
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

/* ------------------------------------------------------------------ */
/* Thread que trata um placar ligado (unica a ler do pipe do placar)   */
/* ------------------------------------------------------------------ */
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

    for (;;) {
        lidos = 0;
        ok = ReadFile(hPipe, buf, sizeof(buf), &lidos, NULL);
        if (!ok || lidos == 0) {
            PrintConsola(ctx, _T("[Central] Placar %lu desligou-se (pipe fechado).\n"),
                ctx->placares[idx].identificador);
            break;
        }
        tipo = buf[0];

        if (tipo == 1) {
            /* ligar — atribuir identificador */
            EnterCriticalSection(&ctx->csPlacares);
            id = ctx->proximoId++;
            ctx->placares[idx].identificador = id;
            LeaveCriticalSection(&ctx->csPlacares);

            /* Enviar MSG_ID de volta */
            MSG_ID mid;
            mid.tipo = 7;
            mid.identificador = id;
            EscreverPipePlacar(ctx, idx, &mid, sizeof(MSG_ID));
            PrintConsola(ctx, _T("[Central] Placar ligado com identificador %lu.\n"), id);

        } else if (tipo == 2) {
            /* desligar — enviar confirmacao e terminar */
            MSG_CMD conf;
            conf.tipo = 2;
            EscreverPipePlacar(ctx, idx, &conf, sizeof(MSG_CMD));
            PrintConsola(ctx, _T("[Central] Placar %lu desligou-se.\n"),
                ctx->placares[idx].identificador);
            break;

        } else if (tipo == 3) {
            /* fim do alerta (duracao expirou no placar) */
            EnterCriticalSection(&ctx->csPlacares);
            ctx->placares[idx].temAlerta = FALSE;
            ctx->placares[idx].msgAlerta[0] = _T('\0');
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Alerta do placar %lu expirou.\n"),
                ctx->placares[idx].identificador);

        } else if (tipo == 4) {
            /* Confirmacao de recepcao de alerta (placar envia MSG_ALERTA de volta) */
            SetEvent(ctx->placares[idx].eventoConfirmacao);

        } else if (tipo == 5) {
            /* Confirmacao de cancelamento */
            SetEvent(ctx->placares[idx].eventoConfirmacao);

        } else {
            PrintConsola(ctx, _T("[Central] Mensagem desconhecida (tipo=%u) do placar %lu.\n"),
                (unsigned)tipo, ctx->placares[idx].identificador);
        }
    }

    /* Limpar slot */
    EnterCriticalSection(&ctx->csPlacares);
    CloseHandle(hPipe);
    ctx->placares[idx].ativo = FALSE;
    ctx->placares[idx].hPipe = NULL;
    ctx->placares[idx].identificador = 0;
    ctx->placares[idx].temAlerta = FALSE;
    ctx->placares[idx].msgAlerta[0] = _T('\0');
    LeaveCriticalSection(&ctx->csPlacares);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread que aceita ligacoes de novos placares                        */
/* ------------------------------------------------------------------ */
static DWORD WINAPI ThreadAceitarLigacoes(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;
    TCHAR nomePipeCompleto[TAM_NOME_PIPE + 10];
    HANDLE hPipe;
    BOOL ligado;
    int slot;
    ARGS_THREAD_PLACAR* args;
    HANDLE hThread;

    _sntprintf(nomePipeCompleto, _countof(nomePipeCompleto),
               _T("\\\\.\\pipe\\%s"), ctx->nomePipe);

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        hPipe = CreateNamedPipe(
            nomePipeCompleto,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            MAX_PLACARES,
            4096, 4096,
            0, NULL
        );
        if (hPipe == INVALID_HANDLE_VALUE) {
            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;
            PrintConsola(ctx, _T("[Central] Erro CreateNamedPipe (%lu)\n"), GetLastError());
            Sleep(500);
            continue;
        }

        ligado = ConnectNamedPipe(hPipe, NULL);
        if (!ligado && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;
            continue;
        }

        if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) {
            CloseHandle(hPipe);
            break;
        }

        EnterCriticalSection(&ctx->csPlacares);
        slot = EncontrarSlotLivre(ctx);
        if (slot < 0) {
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Numero maximo de placares atingido.\n"));
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
        if (args == NULL) {
            EnterCriticalSection(&ctx->csPlacares);
            ctx->placares[slot].ativo = FALSE;
            ctx->placares[slot].hPipe = NULL;
            LeaveCriticalSection(&ctx->csPlacares);
            CloseHandle(hPipe);
            continue;
        }
        args->ctx = ctx;
        args->idx = slot;

        hThread = CreateThread(NULL, 0, ThreadPlacar, args, 0, NULL);
        if (hThread == NULL) {
            free(args);
            EnterCriticalSection(&ctx->csPlacares);
            ctx->placares[slot].ativo = FALSE;
            ctx->placares[slot].hPipe = NULL;
            LeaveCriticalSection(&ctx->csPlacares);
            CloseHandle(hPipe);
            continue;
        }
        CloseHandle(hThread);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Comandos do administrador                                           */
/* ------------------------------------------------------------------ */

/* alerta <msg> <duracao> <id_placar> */
static void CmdAlerta(CONTEXTO_APP* ctx, TCHAR* args) {
    TCHAR copia[TAM_MAX_COMANDO];
    TCHAR* tokens[512];
    TCHAR* ctx_tok;
    TCHAR* t;
    int n, i;
    DWORD duracao, idPlacar;
    TCHAR msg[140];
    MSG_ALERTA alerta;
    int destinos[MAX_PLACARES];
    HANDLE eventos[MAX_PLACARES];
    int nDestinos = 0;

    if (args == NULL || args[0] == _T('\0')) {
        PrintConsola(ctx, _T("Uso: alerta <msg> <duracao> <id_placar>\n"));
        return;
    }

    _tcsncpy_s(copia, _countof(copia), args, _TRUNCATE);
    ctx_tok = NULL;
    n = 0;
    t = _tcstok_s(copia, _T(" \t"), &ctx_tok);
    while (t && n < 512) { tokens[n++] = t; t = _tcstok_s(NULL, _T(" \t"), &ctx_tok); }

    if (n < 3) {
        PrintConsola(ctx, _T("Uso: alerta <msg> <duracao> <id_placar>\n"));
        return;
    }

    if (!ParseDWORDStrict(tokens[n - 2], &duracao) ||
        !ParseDWORDStrict(tokens[n - 1], &idPlacar)) {
        PrintConsola(ctx, _T("Erro de sintaxe: duracao e id_placar devem ser inteiros nao negativos.\n"));
        return;
    }

    msg[0] = _T('\0');
    for (i = 0; i < n - 2; i++) {
        if (i > 0) _tcsncat_s(msg, _countof(msg), _T(" "), _TRUNCATE);
        _tcsncat_s(msg, _countof(msg), tokens[i], _TRUNCATE);
    }

    alerta.tipo = 4;
    _tcsncpy_s(alerta.msg, _countof(alerta.msg), msg, _TRUNCATE);
    alerta.duracao = duracao;

    EnterCriticalSection(&ctx->csPlacares);

    if (idPlacar == 0) {
        for (i = 0; i < MAX_PLACARES; i++) {
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
                _tcsncpy_s(ctx->placares[destinos[i]].msgAlerta, 140, msg, _TRUNCATE);
            }
            LeaveCriticalSection(&ctx->csPlacares);
        }
        PrintConsola(ctx, _T("[Central] Alerta enviado a todos os placares.\n"));
    } else {
        int idx = EncontrarPlacarPorId(ctx, idPlacar);
        if (idx < 0) {
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Placar %lu nao encontrado.\n"), idPlacar);
            return;
        }
        HANDLE evConf = ctx->placares[idx].eventoConfirmacao;
        if (EscreverPipePlacar(ctx, idx, &alerta, sizeof(MSG_ALERTA))) {
            LeaveCriticalSection(&ctx->csPlacares);
            WaitForSingleObject(evConf, 5000);
            EnterCriticalSection(&ctx->csPlacares);
            if (ctx->placares[idx].ativo) {
                ctx->placares[idx].temAlerta = TRUE;
                _tcsncpy_s(ctx->placares[idx].msgAlerta, 140, msg, _TRUNCATE);
            }
            LeaveCriticalSection(&ctx->csPlacares);
        } else {
            LeaveCriticalSection(&ctx->csPlacares);
        }
        PrintConsola(ctx, _T("[Central] Alerta enviado ao placar %lu.\n"), idPlacar);
    }
}

/* cancelar <id_placar> */
static void CmdCancelar(CONTEXTO_APP* ctx, TCHAR* args) {
    DWORD idPlacar;
    int idx;
    HANDLE evConf;
    MSG_CMD cmd;

    if (args == NULL || args[0] == _T('\0')) {
        PrintConsola(ctx, _T("Uso: cancelar <id_placar>\n"));
        return;
    }
    if (!ParseDWORDStrict(args, &idPlacar)) {
        PrintConsola(ctx, _T("Erro de sintaxe: id_placar deve ser inteiro nao negativo.\n"));
        return;
    }

    EnterCriticalSection(&ctx->csPlacares);
    idx = EncontrarPlacarPorId(ctx, idPlacar);
    if (idx < 0) {
        LeaveCriticalSection(&ctx->csPlacares);
        PrintConsola(ctx, _T("[Central] Placar %lu nao encontrado.\n"), idPlacar);
        return;
    }
    if (!ctx->placares[idx].temAlerta) {
        LeaveCriticalSection(&ctx->csPlacares);
        PrintConsola(ctx, _T("[Central] Placar %lu nao tem alerta ativo.\n"), idPlacar);
        return;
    }
    evConf = ctx->placares[idx].eventoConfirmacao;
    cmd.tipo = 5;
    if (EscreverPipePlacar(ctx, idx, &cmd, sizeof(MSG_CMD))) {
        LeaveCriticalSection(&ctx->csPlacares);
        WaitForSingleObject(evConf, 5000);
        EnterCriticalSection(&ctx->csPlacares);
        if (ctx->placares[idx].ativo) {
            ctx->placares[idx].temAlerta = FALSE;
            ctx->placares[idx].msgAlerta[0] = _T('\0');
        }
        LeaveCriticalSection(&ctx->csPlacares);
    } else {
        LeaveCriticalSection(&ctx->csPlacares);
    }
    PrintConsola(ctx, _T("[Central] Alerta cancelado no placar %lu.\n"), idPlacar);
}

/* listar */
static void CmdListar(CONTEXTO_APP* ctx) {
    int i, encontrou;
    EnterCriticalSection(&ctx->csPlacares);
    PrintConsola(ctx, _T("--- Lista de placares ---\n"));
    encontrou = 0;
    for (i = 0; i < MAX_PLACARES; i++) {
        if (!ctx->placares[i].ativo) continue;
        encontrou = 1;
        if (ctx->placares[i].temAlerta) {
            PrintConsola(ctx, _T("  Placar %lu: alerta ativo = '%s'\n"),
                ctx->placares[i].identificador, ctx->placares[i].msgAlerta);
        } else {
            PrintConsola(ctx, _T("  Placar %lu: sem alerta ativo\n"),
                ctx->placares[i].identificador);
        }
    }
    if (!encontrou)
        PrintConsola(ctx, _T("  (nenhum placar ligado)\n"));
    PrintConsola(ctx, _T("-------------------------\n"));
    LeaveCriticalSection(&ctx->csPlacares);
}

/* encerrar */
static void CmdEncerrar(CONTEXTO_APP* ctx) {
    int i;
    MSG_CMD cmd;
    cmd.tipo = 6;

    PrintConsola(ctx, _T("[Central] A encerrar a plataforma...\n"));
    InterlockedExchange(&ctx->deveSair, 1);

    EnterCriticalSection(&ctx->csPlacares);
    for (i = 0; i < MAX_PLACARES; i++) {
        if (!ctx->placares[i].ativo) continue;
        EscreverPipePlacar(ctx, i, &cmd, sizeof(MSG_CMD));
    }
    LeaveCriticalSection(&ctx->csPlacares);

    SetEvent(ctx->eventoParar);
}

/* ------------------------------------------------------------------ */
/* Thread de comandos do administrador                                 */
/* ------------------------------------------------------------------ */
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
        } else if (_tcsicmp(cmd, _T("cancelar")) == 0) {
            CmdCancelar(ctx, restArgs);
        } else if (_tcsicmp(cmd, _T("listar")) == 0) {
            CmdListar(ctx);
        } else if (_tcsicmp(cmd, _T("encerrar")) == 0) {
            CmdEncerrar(ctx);
            break;
        } else {
            PrintConsola(ctx, _T("Comandos: alerta <msg> <duracao> <id> | cancelar <id> | listar | encerrar\n"));
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int _tmain(int argc, TCHAR* argv[]) {
    CONTEXTO_APP ctx;
    HANDLE hThreadLigacoes, hThreadCmds;
    int i;

    if (argc < 2 || argv[1] == NULL || argv[1][0] == _T('\0')) {
        _tprintf(_T("Uso: central.exe <nome_pipe>\n"));
        _tprintf(_T("Exemplo: central.exe tubo  ->  usa \\\\.\\pipe\\tubo\n"));
        return 1;
    }

    ZeroMemory(&ctx, sizeof(ctx));
    _tcsncpy_s(ctx.nomePipe, _countof(ctx.nomePipe), argv[1], _TRUNCATE);
    ctx.proximoId = 1;

    InitializeCriticalSection(&ctx.csPlacares);
    InitializeCriticalSection(&ctx.csConsola);
    for (i = 0; i < MAX_PLACARES; i++) {
        InitializeCriticalSection(&ctx.placares[i].csPipe);
        ctx.placares[i].eventoConfirmacao = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (ctx.placares[i].eventoConfirmacao == NULL) {
            _tprintf(_T("Erro: CreateEvent(eventoConfirmacao) falhou (%lu)\n"), GetLastError());
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
    if (ctx.eventoParar == NULL) {
        _tprintf(_T("Erro: CreateEvent falhou (%lu)\n"), GetLastError());
        for (i = 0; i < MAX_PLACARES; i++) {
            CloseHandle(ctx.placares[i].eventoConfirmacao);
            DeleteCriticalSection(&ctx.placares[i].csPipe);
        }
        DeleteCriticalSection(&ctx.csPlacares);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    PrintConsola(&ctx, _T("[Central] A iniciar. Named pipe: \\\\.\\pipe\\%s\n"), ctx.nomePipe);

    hThreadLigacoes = CreateThread(NULL, 0, ThreadAceitarLigacoes, &ctx, 0, NULL);
    if (hThreadLigacoes == NULL) {
        _tprintf(_T("Erro: CreateThread(ligacoes) falhou (%lu)\n"), GetLastError());
        CloseHandle(ctx.eventoParar);
        for (i = 0; i < MAX_PLACARES; i++) {
            CloseHandle(ctx.placares[i].eventoConfirmacao);
            DeleteCriticalSection(&ctx.placares[i].csPipe);
        }
        DeleteCriticalSection(&ctx.csPlacares);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    hThreadCmds = CreateThread(NULL, 0, ThreadComandos, &ctx, 0, NULL);
    if (hThreadCmds == NULL) {
        _tprintf(_T("Erro: CreateThread(comandos) falhou (%lu)\n"), GetLastError());
        InterlockedExchange(&ctx.deveSair, 1);
        SetEvent(ctx.eventoParar);
        WaitForSingleObject(hThreadLigacoes, 3000);
        CloseHandle(hThreadLigacoes);
        CloseHandle(ctx.eventoParar);
        for (i = 0; i < MAX_PLACARES; i++) {
            CloseHandle(ctx.placares[i].eventoConfirmacao);
            DeleteCriticalSection(&ctx.placares[i].csPipe);
        }
        DeleteCriticalSection(&ctx.csPlacares);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    WaitForSingleObject(hThreadCmds, INFINITE);

    InterlockedExchange(&ctx.deveSair, 1);
    SetEvent(ctx.eventoParar);
    WaitForSingleObject(hThreadLigacoes, 3000);

    CloseHandle(hThreadCmds);
    CloseHandle(hThreadLigacoes);
    CloseHandle(ctx.eventoParar);

    EnterCriticalSection(&ctx.csPlacares);
    for (i = 0; i < MAX_PLACARES; i++) {
        if (ctx.placares[i].ativo && ctx.placares[i].hPipe != NULL) {
            CloseHandle(ctx.placares[i].hPipe);
            ctx.placares[i].hPipe = NULL;
        }
        if (ctx.placares[i].eventoConfirmacao)
            CloseHandle(ctx.placares[i].eventoConfirmacao);
        DeleteCriticalSection(&ctx.placares[i].csPipe);
    }
    LeaveCriticalSection(&ctx.csPlacares);

    PrintConsola(&ctx, _T("[Central] Encerrado.\n"));
    DeleteCriticalSection(&ctx.csPlacares);
    DeleteCriticalSection(&ctx.csConsola);
    return 0;
}
