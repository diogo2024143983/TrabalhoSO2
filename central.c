#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>

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
    BYTE tipo;          /* 7 - atribuir identificador */
    DWORD identificador;
} MSG_ID;

/* ------------------------------------------------------------------ */
/* Estado de cada placar                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    BOOL  ativo;
    DWORD identificador;
    HANDLE hPipe;           /* handle do named pipe para este placar */
    BOOL  temAlerta;
    TCHAR msgAlerta[140];
    CRITICAL_SECTION csPipe; /* protege escritas/leituras no pipe deste placar */
} ESTADO_PLACAR;

/* ------------------------------------------------------------------ */
/* Contexto global da aplicação                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    TCHAR nomePipe[TAM_NOME_PIPE];
    HANDLE hPipeServidor;       /* pipe de escuta (ConnectNamedPipe) */

    ESTADO_PLACAR placares[MAX_PLACARES];
    DWORD proximoId;
    CRITICAL_SECTION csPlacares; /* protege o array de placares */

    volatile LONG deveSair;
    HANDLE eventoParar;         /* sinalizado quando se quer encerrar */

    CRITICAL_SECTION csConsola; /* protege _tprintf */
} CONTEXTO_APP;

/* ------------------------------------------------------------------ */
/* Utilitários                                                         */
/* ------------------------------------------------------------------ */
static void PrintConsola(CONTEXTO_APP* ctx, const TCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EnterCriticalSection(&ctx->csConsola);
    _vtprintf(fmt, args);
    LeaveCriticalSection(&ctx->csConsola);
    va_end(args);
}

/* Devolve índice do placar com dado identificador, ou -1 */
static int EncontrarPlacarPorId(CONTEXTO_APP* ctx, DWORD id) {
    for (int i = 0; i < MAX_PLACARES; i++) {
        if (ctx->placares[i].ativo && ctx->placares[i].identificador == id)
            return i;
    }
    return -1;
}

/* Devolve índice de slot livre, ou -1 */
static int EncontrarSlotLivre(CONTEXTO_APP* ctx) {
    for (int i = 0; i < MAX_PLACARES; i++) {
        if (!ctx->placares[i].ativo)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Envio de mensagens ao placar (com lock do pipe individual)          */
/* ------------------------------------------------------------------ */
static BOOL EnviarMsgAlerta(CONTEXTO_APP* ctx, int idx, const MSG_ALERTA* alerta) {
    DWORD escritos = 0;
    EnterCriticalSection(&ctx->placares[idx].csPipe);
    BOOL ok = WriteFile(ctx->placares[idx].hPipe, alerta, sizeof(MSG_ALERTA), &escritos, NULL);
    LeaveCriticalSection(&ctx->placares[idx].csPipe);
    return ok && escritos == sizeof(MSG_ALERTA);
}

static BOOL EnviarMsgCmd(CONTEXTO_APP* ctx, int idx, BYTE tipo) {
    MSG_CMD cmd;
    cmd.tipo = tipo;
    DWORD escritos = 0;
    EnterCriticalSection(&ctx->placares[idx].csPipe);
    BOOL ok = WriteFile(ctx->placares[idx].hPipe, &cmd, sizeof(MSG_CMD), &escritos, NULL);
    LeaveCriticalSection(&ctx->placares[idx].csPipe);
    return ok && escritos == sizeof(MSG_CMD);
}

static BOOL EnviarMsgId(CONTEXTO_APP* ctx, int idx, DWORD id) {
    MSG_ID mid;
    mid.tipo = 7;
    mid.identificador = id;
    DWORD escritos = 0;
    EnterCriticalSection(&ctx->placares[idx].csPipe);
    BOOL ok = WriteFile(ctx->placares[idx].hPipe, &mid, sizeof(MSG_ID), &escritos, NULL);
    LeaveCriticalSection(&ctx->placares[idx].csPipe);
    return ok && escritos == sizeof(MSG_ID);
}

/* ------------------------------------------------------------------ */
/* Thread que trata um placar ligado                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    CONTEXTO_APP* ctx;
    int           idx;      /* índice no array de placares */
} ARGS_THREAD_PLACAR;

static DWORD WINAPI ThreadPlacar(LPVOID param) {
    ARGS_THREAD_PLACAR* args = (ARGS_THREAD_PLACAR*)param;
    CONTEXTO_APP* ctx = args->ctx;
    int idx = args->idx;
    free(args);

    HANDLE hPipe = ctx->placares[idx].hPipe;

    for (;;) {
        /* Lê o tipo da mensagem primeiro (1 byte) */
        BYTE tipo = 0;
        DWORD lidos = 0;
        BOOL ok = ReadFile(hPipe, &tipo, sizeof(BYTE), &lidos, NULL);
        if (!ok || lidos == 0) {
            /* Pipe fechado / erro — placar desligou-se abruptamente */
            PrintConsola(ctx, _T("[Central] Placar %lu desligou-se (pipe fechado).\n"),
                ctx->placares[idx].identificador);
            break;
        }

        if (tipo == 1) {
            /* ligar — atribuir identificador */
            EnterCriticalSection(&ctx->csPlacares);
            DWORD id = ctx->proximoId++;
            ctx->placares[idx].identificador = id;
            LeaveCriticalSection(&ctx->csPlacares);

            EnviarMsgId(ctx, idx, id);
            PrintConsola(ctx, _T("[Central] Placar ligado com identificador %lu.\n"), id);

        } else if (tipo == 2) {
            /* desligar */
            EnviarMsgCmd(ctx, idx, 2);
            PrintConsola(ctx, _T("[Central] Placar %lu desligou-se.\n"),
                ctx->placares[idx].identificador);
            break;

        } else if (tipo == 3) {
            /* fim do alerta (duração expirou no placar) */
            EnterCriticalSection(&ctx->csPlacares);
            ctx->placares[idx].temAlerta = FALSE;
            ctx->placares[idx].msgAlerta[0] = _T('\0');
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Alerta do placar %lu expirou.\n"),
                ctx->placares[idx].identificador);

        } else {
            PrintConsola(ctx, _T("[Central] Mensagem desconhecida (tipo=%u) do placar %lu.\n"),
                (unsigned)tipo, ctx->placares[idx].identificador);
        }
    }

    /* Limpar slot */
    EnterCriticalSection(&ctx->csPlacares);
    DeleteCriticalSection(&ctx->placares[idx].csPipe);
    CloseHandle(hPipe);
    ctx->placares[idx].ativo = FALSE;
    ctx->placares[idx].hPipe = NULL;
    ctx->placares[idx].temAlerta = FALSE;
    ctx->placares[idx].msgAlerta[0] = _T('\0');
    LeaveCriticalSection(&ctx->csPlacares);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread que aceita ligações de novos placares                        */
/* ------------------------------------------------------------------ */
static DWORD WINAPI ThreadAceitarLigacoes(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;

    TCHAR nomePipeCompleto[TAM_NOME_PIPE + 10];
    _sntprintf(nomePipeCompleto, _countof(nomePipeCompleto), _T("\\\\.\\pipe\\%s"), ctx->nomePipe);

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        /* Criar instância do named pipe (message mode, bidirecional) */
        HANDLE hPipe = CreateNamedPipe(
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

        /* Aguardar ligação de um cliente */
        BOOL ligado = ConnectNamedPipe(hPipe, NULL);
        if (!ligado && GetLastError() != ERROR_PIPE_CONNECTED) {
            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) {
                CloseHandle(hPipe);
                break;
            }
            CloseHandle(hPipe);
            continue;
        }

        if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) {
            CloseHandle(hPipe);
            break;
        }

        /* Encontrar slot livre */
        EnterCriticalSection(&ctx->csPlacares);
        int slot = EncontrarSlotLivre(ctx);
        if (slot < 0) {
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Numero maximo de placares atingido. Ligacao recusada.\n"));
            CloseHandle(hPipe);
            continue;
        }
        ctx->placares[slot].ativo = TRUE;
        ctx->placares[slot].hPipe = hPipe;
        ctx->placares[slot].identificador = 0;
        ctx->placares[slot].temAlerta = FALSE;
        ctx->placares[slot].msgAlerta[0] = _T('\0');
        InitializeCriticalSection(&ctx->placares[slot].csPipe);
        LeaveCriticalSection(&ctx->csPlacares);

        /* Lançar thread para tratar este placar */
        ARGS_THREAD_PLACAR* args = (ARGS_THREAD_PLACAR*)malloc(sizeof(ARGS_THREAD_PLACAR));
        if (args == NULL) {
            EnterCriticalSection(&ctx->csPlacares);
            DeleteCriticalSection(&ctx->placares[slot].csPipe);
            ctx->placares[slot].ativo = FALSE;
            ctx->placares[slot].hPipe = NULL;
            LeaveCriticalSection(&ctx->csPlacares);
            CloseHandle(hPipe);
            continue;
        }
        args->ctx = ctx;
        args->idx = slot;

        HANDLE hThread = CreateThread(NULL, 0, ThreadPlacar, args, 0, NULL);
        if (hThread == NULL) {
            free(args);
            EnterCriticalSection(&ctx->csPlacares);
            DeleteCriticalSection(&ctx->placares[slot].csPipe);
            ctx->placares[slot].ativo = FALSE;
            ctx->placares[slot].hPipe = NULL;
            LeaveCriticalSection(&ctx->csPlacares);
            CloseHandle(hPipe);
            continue;
        }
        CloseHandle(hThread); /* não precisamos de esperar por ela aqui */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Processamento dos comandos do administrador                         */
/* ------------------------------------------------------------------ */

/* alerta <msg> <duracao> <id_placar> */
static void CmdAlerta(CONTEXTO_APP* ctx, TCHAR* args) {
    /* Formato: "msg" duracao id  — a mensagem pode ter espaços se entre aspas,
       mas o enunciado não especifica aspas, por isso lemos até ao último token */

    /* Estratégia: os dois últimos tokens são duracao e id; o resto é a mensagem */
    if (args == NULL || args[0] == _T('\0')) {
        PrintConsola(ctx, _T("Uso: alerta <msg> <duracao> <id_placar>\n"));
        return;
    }

    /* Encontrar os dois últimos tokens */
    TCHAR* ultimo = NULL;
    TCHAR* penultimo = NULL;
    TCHAR* p = args;
    TCHAR* tok = NULL;
    TCHAR* ctx_tok = NULL;

    /* Fazer uma cópia para tokenizar e encontrar posições */
    TCHAR copia[TAM_MAX_COMANDO];
    _tcsncpy_s(copia, _countof(copia), args, _TRUNCATE);

    /* Contar tokens */
    int nToks = 0;
    TCHAR* t = _tcstok_s(copia, _T(" \t"), &ctx_tok);
    while (t) { nToks++; t = _tcstok_s(NULL, _T(" \t"), &ctx_tok); }

    if (nToks < 3) {
        PrintConsola(ctx, _T("Uso: alerta <msg> <duracao> <id_placar>\n"));
        return;
    }

    /* Reconstruir: encontrar posição do penúltimo token no args original */
    /* Vamos tokenizar de novo */
    _tcsncpy_s(copia, _countof(copia), args, _TRUNCATE);
    ctx_tok = NULL;
    TCHAR* tokens[512];
    int n = 0;
    t = _tcstok_s(copia, _T(" \t"), &ctx_tok);
    while (t && n < 512) { tokens[n++] = t; t = _tcstok_s(NULL, _T(" \t"), &ctx_tok); }

    DWORD duracao = (DWORD)_ttoi(tokens[n - 2]);
    DWORD idPlacar = (DWORD)_ttoi(tokens[n - 1]);

    /* Mensagem: tudo antes dos dois últimos tokens */
    /* Calcular comprimento da mensagem no args original */
    /* Posição do token n-2 no args: procurar a partir do início */
    TCHAR msg[140];
    ZeroMemory(msg, sizeof(msg));

    /* Reconstruir mensagem juntando tokens[0..n-3] */
    msg[0] = _T('\0');
    for (int i = 0; i < n - 2; i++) {
        if (i > 0) _tcsncat_s(msg, _countof(msg), _T(" "), _TRUNCATE);
        _tcsncat_s(msg, _countof(msg), tokens[i], _TRUNCATE);
    }

    MSG_ALERTA alerta;
    alerta.tipo = 4;
    _tcsncpy_s(alerta.msg, _countof(alerta.msg), msg, _TRUNCATE);
    alerta.duracao = duracao;

    EnterCriticalSection(&ctx->csPlacares);

    if (idPlacar == 0) {
        /* Enviar a todos */
        for (int i = 0; i < MAX_PLACARES; i++) {
            if (!ctx->placares[i].ativo) continue;
            if (EnviarMsgAlerta(ctx, i, &alerta)) {
                /* Aguardar confirmação */
                MSG_ALERTA conf;
                DWORD lidos = 0;
                EnterCriticalSection(&ctx->placares[i].csPipe);
                ReadFile(ctx->placares[i].hPipe, &conf, sizeof(MSG_ALERTA), &lidos, NULL);
                LeaveCriticalSection(&ctx->placares[i].csPipe);
                ctx->placares[i].temAlerta = TRUE;
                _tcsncpy_s(ctx->placares[i].msgAlerta, 140, msg, _TRUNCATE);
            }
        }
        LeaveCriticalSection(&ctx->csPlacares);
        PrintConsola(ctx, _T("[Central] Alerta enviado a todos os placares.\n"));
    } else {
        int idx = EncontrarPlacarPorId(ctx, idPlacar);
        if (idx < 0) {
            LeaveCriticalSection(&ctx->csPlacares);
            PrintConsola(ctx, _T("[Central] Placar %lu nao encontrado.\n"), idPlacar);
            return;
        }
        if (EnviarMsgAlerta(ctx, idx, &alerta)) {
            /* Aguardar confirmação */
            MSG_ALERTA conf;
            DWORD lidos = 0;
            EnterCriticalSection(&ctx->placares[idx].csPipe);
            ReadFile(ctx->placares[idx].hPipe, &conf, sizeof(MSG_ALERTA), &lidos, NULL);
            LeaveCriticalSection(&ctx->placares[idx].csPipe);
            ctx->placares[idx].temAlerta = TRUE;
            _tcsncpy_s(ctx->placares[idx].msgAlerta, 140, msg, _TRUNCATE);
        }
        LeaveCriticalSection(&ctx->csPlacares);
        PrintConsola(ctx, _T("[Central] Alerta enviado ao placar %lu.\n"), idPlacar);
    }
}

/* cancelar <id_placar> */
static void CmdCancelar(CONTEXTO_APP* ctx, TCHAR* args) {
    if (args == NULL || args[0] == _T('\0')) {
        PrintConsola(ctx, _T("Uso: cancelar <id_placar>\n"));
        return;
    }
    DWORD idPlacar = (DWORD)_ttoi(args);

    EnterCriticalSection(&ctx->csPlacares);
    int idx = EncontrarPlacarPorId(ctx, idPlacar);
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
    if (EnviarMsgCmd(ctx, idx, 5)) {
        /* Aguardar confirmação */
        MSG_CMD conf;
        DWORD lidos = 0;
        EnterCriticalSection(&ctx->placares[idx].csPipe);
        ReadFile(ctx->placares[idx].hPipe, &conf, sizeof(MSG_CMD), &lidos, NULL);
        LeaveCriticalSection(&ctx->placares[idx].csPipe);
        ctx->placares[idx].temAlerta = FALSE;
        ctx->placares[idx].msgAlerta[0] = _T('\0');
    }
    LeaveCriticalSection(&ctx->csPlacares);
    PrintConsola(ctx, _T("[Central] Alerta cancelado no placar %lu.\n"), idPlacar);
}

/* listar */
static void CmdListar(CONTEXTO_APP* ctx) {
    EnterCriticalSection(&ctx->csPlacares);
    PrintConsola(ctx, _T("--- Lista de placares ---\n"));
    int encontrou = 0;
    for (int i = 0; i < MAX_PLACARES; i++) {
        if (!ctx->placares[i].ativo) continue;
        encontrou = 1;
        if (ctx->placares[i].temAlerta) {
            PrintConsola(ctx, _T("  Placar %lu: alerta ativo = '%s'\n"),
                ctx->placares[i].identificador,
                ctx->placares[i].msgAlerta);
        } else {
            PrintConsola(ctx, _T("  Placar %lu: sem alerta ativo\n"),
                ctx->placares[i].identificador);
        }
    }
    if (!encontrou) {
        PrintConsola(ctx, _T("  (nenhum placar ligado)\n"));
    }
    PrintConsola(ctx, _T("-------------------------\n"));
    LeaveCriticalSection(&ctx->csPlacares);
}

/* encerrar */
static void CmdEncerrar(CONTEXTO_APP* ctx) {
    PrintConsola(ctx, _T("[Central] A encerrar a plataforma...\n"));
    InterlockedExchange(&ctx->deveSair, 1);

    EnterCriticalSection(&ctx->csPlacares);
    for (int i = 0; i < MAX_PLACARES; i++) {
        if (!ctx->placares[i].ativo) continue;
        EnviarMsgCmd(ctx, i, 6); /* encerrar — sem confirmação */
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

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        EnterCriticalSection(&ctx->csConsola);
        _tprintf(_T("CMD> "));
        LeaveCriticalSection(&ctx->csConsola);

        if (_fgetts(linha, _countof(linha), stdin) == NULL) {
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            break;
        }

        /* Remover \n / \r */
        size_t len = _tcslen(linha);
        while (len > 0 && (linha[len - 1] == _T('\n') || linha[len - 1] == _T('\r')))
            linha[--len] = _T('\0');

        if (len == 0) continue;

        /* Separar comando dos argumentos */
        TCHAR* ctx_tok = NULL;
        TCHAR* cmd = _tcstok_s(linha, _T(" \t"), &ctx_tok);
        if (cmd == NULL) continue;

        /* ctx_tok aponta para o resto da linha (argumentos) */
        TCHAR* restArgs = ctx_tok; /* pode ser NULL ou string vazia */

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
    if (argc < 2 || argv[1] == NULL || argv[1][0] == _T('\0')) {
        _tprintf(_T("Uso: central.exe <nome_pipe>\n"));
        _tprintf(_T("Exemplo: central.exe tubo  ->  usa \\\\.\\pipe\\tubo\n"));
        return 1;
    }

    CONTEXTO_APP ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    _tcsncpy_s(ctx.nomePipe, _countof(ctx.nomePipe), argv[1], _TRUNCATE);
    ctx.proximoId = 1;

    InitializeCriticalSection(&ctx.csPlacares);
    InitializeCriticalSection(&ctx.csConsola);

    ctx.eventoParar = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ctx.eventoParar == NULL) {
        _tprintf(_T("Erro: CreateEvent falhou (%lu)\n"), GetLastError());
        DeleteCriticalSection(&ctx.csPlacares);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    PrintConsola(&ctx, _T("[Central] A iniciar. Named pipe: \\\\.\\pipe\\%s\n"), ctx.nomePipe);

    /* Thread que aceita ligações de placares */
    HANDLE hThreadLigacoes = CreateThread(NULL, 0, ThreadAceitarLigacoes, &ctx, 0, NULL);
    if (hThreadLigacoes == NULL) {
        _tprintf(_T("Erro: CreateThread(ligacoes) falhou (%lu)\n"), GetLastError());
        CloseHandle(ctx.eventoParar);
        DeleteCriticalSection(&ctx.csPlacares);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    /* Thread de comandos do administrador */
    HANDLE hThreadCmds = CreateThread(NULL, 0, ThreadComandos, &ctx, 0, NULL);
    if (hThreadCmds == NULL) {
        _tprintf(_T("Erro: CreateThread(comandos) falhou (%lu)\n"), GetLastError());
        InterlockedExchange(&ctx.deveSair, 1);
        SetEvent(ctx.eventoParar);
        WaitForSingleObject(hThreadLigacoes, 3000);
        CloseHandle(hThreadLigacoes);
        CloseHandle(ctx.eventoParar);
        DeleteCriticalSection(&ctx.csPlacares);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    /* Aguardar que o administrador encerre */
    WaitForSingleObject(hThreadCmds, INFINITE);

    /* Sinalizar encerramento e aguardar thread de ligações */
    InterlockedExchange(&ctx.deveSair, 1);
    SetEvent(ctx.eventoParar);

    /* Fechar o pipe servidor para desbloquear ConnectNamedPipe */
    /* A thread de ligações vai sair no próximo ciclo */
    WaitForSingleObject(hThreadLigacoes, 3000);

    CloseHandle(hThreadCmds);
    CloseHandle(hThreadLigacoes);
    CloseHandle(ctx.eventoParar);

    /* Fechar pipes de placares ainda ligados */
    EnterCriticalSection(&ctx.csPlacares);
    for (int i = 0; i < MAX_PLACARES; i++) {
        if (ctx.placares[i].ativo && ctx.placares[i].hPipe != NULL) {
            CloseHandle(ctx.placares[i].hPipe);
            ctx.placares[i].hPipe = NULL;
            DeleteCriticalSection(&ctx.placares[i].csPipe);
        }
    }
    LeaveCriticalSection(&ctx.csPlacares);

    DeleteCriticalSection(&ctx.csPlacares);
    DeleteCriticalSection(&ctx.csConsola);

    PrintConsola(&ctx, _T("[Central] Encerrado.\n"));
    return 0;
}
