#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */
#define CHAVE_REGISTO   _T("Software\\TrabSO2")
#define VALOR_NPIPE     _T("NPIPE")
#define TAM_MAX_COMANDO 256
#define TAM_NOME_PIPE   256
#define PREFIXO_PIPE    _T("\\\\.\\pipe\\")

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
/* Contexto da aplicacao                                               */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Utilitarios                                                         */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */
static BOOL GarantirNomePipeNoRegisto(const TCHAR* daLinhaComandos,
                                       TCHAR* nomePipeSaida, DWORD tamanhoSaida) {
    HKEY chave = NULL;
    LONG res;
    DWORD tipo = 0;
    DWORD bytesDados = tamanhoSaida * sizeof(TCHAR);

    res = RegCreateKeyEx(HKEY_CURRENT_USER, CHAVE_REGISTO, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                         NULL, &chave, NULL);
    if (res != ERROR_SUCCESS) {
        _tprintf(_T("Erro: nao foi possivel aceder ao Registry (%ld)\n"), res);
        return FALSE;
    }

    if (daLinhaComandos != NULL && daLinhaComandos[0] != _T('\0')) {
        _tcsncpy_s(nomePipeSaida, tamanhoSaida, daLinhaComandos, _TRUNCATE);
        res = RegSetValueEx(chave, VALOR_NPIPE, 0, REG_SZ,
                            (const BYTE*)nomePipeSaida,
                            (DWORD)((_tcslen(nomePipeSaida) + 1) * sizeof(TCHAR)));
        RegCloseKey(chave);
        if (res != ERROR_SUCCESS) {
            _tprintf(_T("Erro: nao foi possivel guardar NPIPE no Registry (%ld)\n"), res);
            return FALSE;
        }
        return TRUE;
    }

    res = RegQueryValueEx(chave, VALOR_NPIPE, NULL, &tipo,
                          (LPBYTE)nomePipeSaida, &bytesDados);
    RegCloseKey(chave);

    if (res != ERROR_SUCCESS || tipo != REG_SZ || nomePipeSaida[0] == _T('\0')) {
        _tprintf(_T("[ERRO] O nome do 'NPIPE' nao foi especificado (args ou registry)!\n"));
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Escrita no pipe (protegida por csPipe)                              */
/* ------------------------------------------------------------------ */
static BOOL EscreverPipe(CONTEXTO_APP* ctx, const void* dados, DWORD tam) {
    DWORD escritos = 0;
    EnterCriticalSection(&ctx->csPipe);
    BOOL ok = WriteFile(ctx->hPipe, dados, tam, &escritos, NULL);
    LeaveCriticalSection(&ctx->csPipe);
    return ok && escritos == tam;
}

/* ------------------------------------------------------------------ */
/* Thread que gere o temporizador do alerta ativo                      */
/* ------------------------------------------------------------------ */
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
    if (timer == NULL) {
        PrintConsola(ctx, _T("Erro: CreateWaitableTimer falhou (%lu)\n"), GetLastError());
        return 1;
    }

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
        if (!SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE)) {
            PrintConsola(ctx, _T("Aviso: SetWaitableTimer falhou (%lu)\n"), GetLastError());
            continue;
        }

        espDuracao[0] = ctx->eventoParar;
        espDuracao[1] = ctx->eventoNovoAlerta;
        espDuracao[2] = ctx->eventoCancelarAlerta;
        espDuracao[3] = timer;

        for (;;) {
            resDuracao = WaitForMultipleObjects(4, espDuracao, FALSE, INFINITE);
            if (resDuracao == WAIT_OBJECT_0) {
                CloseHandle(timer);
                return 0;
            } else if (resDuracao == WAIT_OBJECT_0 + 1) {
                EnterCriticalSection(&ctx->csAlerta);
                alerta = ctx->alertaAtivo;
                LeaveCriticalSection(&ctx->csAlerta);
                alerta.msg[_countof(alerta.msg) - 1] = _T('\0');
                ImprimirComTimestamp(ctx, alerta.msg);
                li.QuadPart = -((LONGLONG)alerta.duracao * 10000000LL);
                SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
            } else if (resDuracao == WAIT_OBJECT_0 + 2) {
                CancelWaitableTimer(timer);
                ImprimirComTimestamp(ctx, _T("---"));
                break;
            } else if (resDuracao == WAIT_OBJECT_0 + 3) {
                ImprimirComTimestamp(ctx, _T("---"));
                cmd3.tipo = 3;
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

/* ------------------------------------------------------------------ */
/* Thread que recebe mensagens do central (unica thread a ler do pipe) */
/* ------------------------------------------------------------------ */
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
                PrintConsola(ctx, _T("[Placar] Ligacao ao central perdida.\n"));
                InterlockedExchange(&ctx->deveSair, 1);
                SetEvent(ctx->eventoParar);
                SetEvent(ctx->eventoRespostaLigar);
                SetEvent(ctx->eventoRespostaDesligar);
            }
            break;
        }

        tipo = buf[0];

        if (tipo == 4) {
            if (lidos < sizeof(MSG_ALERTA)) {
                PrintConsola(ctx, _T("[Placar] Mensagem de alerta incompleta (%lu bytes).\n"), lidos);
                continue;
            }
            alerta = (MSG_ALERTA*)buf;
            alerta->msg[_countof(alerta->msg) - 1] = _T('\0');

            EnterCriticalSection(&ctx->csAlerta);
            ctx->alertaAtivo = *alerta;
            ctx->temAlerta = 1;
            LeaveCriticalSection(&ctx->csAlerta);

            EscreverPipe(ctx, alerta, sizeof(MSG_ALERTA));
            SetEvent(ctx->eventoNovoAlerta);

        } else if (tipo == 5) {
            conf.tipo = 5;
            EscreverPipe(ctx, &conf, sizeof(MSG_CMD));

            EnterCriticalSection(&ctx->csAlerta);
            ctx->temAlerta = 0;
            LeaveCriticalSection(&ctx->csAlerta);

            SetEvent(ctx->eventoCancelarAlerta);

        } else if (tipo == 6) {
            PrintConsola(ctx, _T("[Placar] Central encerrou a plataforma. A terminar...\n"));
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            SetEvent(ctx->eventoRespostaLigar);
            SetEvent(ctx->eventoRespostaDesligar);
            ExitProcess(0);

        } else if (tipo == 7) {
            if (lidos >= sizeof(MSG_ID)) {
                mid = (MSG_ID*)buf;
                ctx->identificador = mid->identificador;
            }
            SetEvent(ctx->eventoRespostaLigar);

        } else if (tipo == 2) {
            SetEvent(ctx->eventoRespostaDesligar);

        } else {
            PrintConsola(ctx, _T("[Placar] Mensagem desconhecida do central (tipo=%u).\n"),
                         (unsigned)tipo);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread de comandos do instalador                                    */
/* ------------------------------------------------------------------ */
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

        if (_tcsicmp(comando, _T("liga")) == 0) {
            if (InterlockedCompareExchange(&ctx->ligado, 0, 0) != 0) {
                PrintConsola(ctx, _T("Placar ja se encontra ligado.\n"));
                continue;
            }

            cmd.tipo = 1;
            if (!EscreverPipe(ctx, &cmd, sizeof(MSG_CMD))) {
                PrintConsola(ctx, _T("[Placar] Erro ao enviar pedido de ligar ao central.\n"));
                continue;
            }

            espLigar[0] = ctx->eventoParar;
            espLigar[1] = ctx->eventoRespostaLigar;
            res = WaitForMultipleObjects(2, espLigar, FALSE, 5000);

            if (res != WAIT_OBJECT_0 + 1) {
                PrintConsola(ctx, _T("[Placar] Timeout ou erro a aguardar resposta do central.\n"));
                continue;
            }

            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;

            InterlockedExchange(&ctx->ligado, 1);
            PrintConsola(ctx, _T("Identificador = %lu\n"), ctx->identificador);

        } else if (_tcsicmp(comando, _T("desliga")) == 0) {
            if (InterlockedCompareExchange(&ctx->ligado, 0, 0) == 0) {
                PrintConsola(ctx, _T("A terminar...\n"));
                InterlockedExchange(&ctx->deveSair, 1);
                SetEvent(ctx->eventoParar);
                break;
            }

            cmd.tipo = 2;
            EscreverPipe(ctx, &cmd, sizeof(MSG_CMD));

            espDesligar[0] = ctx->eventoParar;
            espDesligar[1] = ctx->eventoRespostaDesligar;
            WaitForMultipleObjects(2, espDesligar, FALSE, 5000);

            PrintConsola(ctx, _T("A terminar...\n"));
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            break;

        } else {
            PrintConsola(ctx, _T("Comandos disponiveis: liga | desliga\n"));
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
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

    PrintConsola(&ctx, _T("NamedPipe = '%s'\n"), ctx.nomePipe);

    _sntprintf(nomePipeCompleto, _countof(nomePipeCompleto),
               _T("%s%s"), PREFIXO_PIPE, ctx.nomePipe);

    PrintConsola(&ctx, _T("[Placar] A ligar ao central em '%s'...\n"), nomePipeCompleto);

    ligadoPipe = FALSE;
    for (tentativa = 0; tentativa < 5; tentativa++) {
        ctx.hPipe = CreateFile(nomePipeCompleto, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (ctx.hPipe != INVALID_HANDLE_VALUE) {
            ligadoPipe = TRUE;
            break;
        }
        err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipe(nomePipeCompleto, 2000);
        } else {
            PrintConsola(&ctx, _T("[Placar] Erro ao ligar ao pipe (%lu). Tentativa %d/5...\n"),
                         err, tentativa + 1);
            Sleep(1000);
        }
    }

    if (!ligadoPipe) {
        PrintConsola(&ctx, _T("[Placar] Nao foi possivel ligar ao central.\n"));
        DeleteCriticalSection(&ctx.csConsola);
        DeleteCriticalSection(&ctx.csPipe);
        DeleteCriticalSection(&ctx.csAlerta);
        return 1;
    }

    modoLeitura = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(ctx.hPipe, &modoLeitura, NULL, NULL);

    PrintConsola(&ctx, _T("[Placar] Ligado ao central. Use 'liga' para registar o placar.\n"));

    ctx.eventoParar            = CreateEvent(NULL, TRUE,  FALSE, NULL);
    ctx.eventoNovoAlerta       = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx.eventoCancelarAlerta   = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx.eventoRespostaLigar    = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx.eventoRespostaDesligar = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!ctx.eventoParar || !ctx.eventoNovoAlerta || !ctx.eventoCancelarAlerta
        || !ctx.eventoRespostaLigar || !ctx.eventoRespostaDesligar) {
        PrintConsola(&ctx, _T("Erro: CreateEvent falhou (%lu)\n"), GetLastError());
        if (ctx.eventoParar)            CloseHandle(ctx.eventoParar);
        if (ctx.eventoNovoAlerta)       CloseHandle(ctx.eventoNovoAlerta);
        if (ctx.eventoCancelarAlerta)   CloseHandle(ctx.eventoCancelarAlerta);
        if (ctx.eventoRespostaLigar)    CloseHandle(ctx.eventoRespostaLigar);
        if (ctx.eventoRespostaDesligar) CloseHandle(ctx.eventoRespostaDesligar);
        CloseHandle(ctx.hPipe);
        DeleteCriticalSection(&ctx.csConsola);
        DeleteCriticalSection(&ctx.csPipe);
        DeleteCriticalSection(&ctx.csAlerta);
        return 1;
    }

    hThreadCmds    = CreateThread(NULL, 0, ThreadComandos,       &ctx, 0, NULL);
    hThreadAlertas = CreateThread(NULL, 0, ThreadAlertas,        &ctx, 0, NULL);
    hThreadCentral = CreateThread(NULL, 0, ThreadReceberCentral, &ctx, 0, NULL);

    if (!hThreadCmds || !hThreadAlertas || !hThreadCentral) {
        PrintConsola(&ctx, _T("Erro: falha a criar threads (%lu)\n"), GetLastError());
        InterlockedExchange(&ctx.deveSair, 1);
        SetEvent(ctx.eventoParar);
        if (hThreadCmds)    { WaitForSingleObject(hThreadCmds,    2000); CloseHandle(hThreadCmds); }
        if (hThreadAlertas) { WaitForSingleObject(hThreadAlertas, 2000); CloseHandle(hThreadAlertas); }
        if (hThreadCentral) { WaitForSingleObject(hThreadCentral, 2000); CloseHandle(hThreadCentral); }
        CloseHandle(ctx.eventoParar);
        CloseHandle(ctx.eventoNovoAlerta);
        CloseHandle(ctx.eventoCancelarAlerta);
        CloseHandle(ctx.eventoRespostaLigar);
        CloseHandle(ctx.eventoRespostaDesligar);
        CloseHandle(ctx.hPipe);
        DeleteCriticalSection(&ctx.csConsola);
        DeleteCriticalSection(&ctx.csPipe);
        DeleteCriticalSection(&ctx.csAlerta);
        return 1;
    }

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
