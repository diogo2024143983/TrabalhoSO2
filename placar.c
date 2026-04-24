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
/* Contexto da aplicação                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    TCHAR  nomePipe[TAM_NOME_PIPE];     /* nome simples (sem prefixo) */
    HANDLE hPipe;                       /* handle do named pipe ao central */
    DWORD  identificador;               /* atribuído pelo central */
    volatile LONG ligado;               /* 1 se ligado ao central */
    volatile LONG deveSair;             /* 1 para terminar */

    /* Alerta ativo (protegido por csAlerta) */
    volatile LONG temAlerta;
    MSG_ALERTA alertaAtivo;

    /* Sincronização */
    CRITICAL_SECTION csConsola;         /* protege _tprintf */
    CRITICAL_SECTION csPipe;            /* protege escritas no pipe */
    CRITICAL_SECTION csAlerta;          /* protege alertaAtivo */

    /* Eventos */
    HANDLE eventoParar;                 /* manual-reset: sinalizado para terminar */
    HANDLE eventoNovoAlerta;            /* auto-reset: novo alerta recebido */
    HANDLE eventoCancelarAlerta;        /* auto-reset: alerta cancelado pelo central */
    HANDLE eventoRespostaLigar;         /* auto-reset: central respondeu ao ligar */
    HANDLE eventoRespostaDesligar;      /* auto-reset: central confirmou desligar */
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

static void ImprimirComTimestamp(CONTEXTO_APP* ctx, const TCHAR* texto) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&ctx->csConsola);
    _tprintf(_T("%02u/%02u/%04u (%02u:%02u:%02u): '%s'\n"),
        st.wDay, st.wMonth, st.wYear,
        st.wHour, st.wMinute, st.wSecond,
        texto);
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
/* Toda a lógica de display e temporização fica aqui.                  */
/* ------------------------------------------------------------------ */
static DWORD WINAPI ThreadAlertas(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;

    for (;;) {
        /* Aguardar novo alerta ou ordem de parar */
        HANDLE espNovoAlerta[2] = { ctx->eventoParar, ctx->eventoNovoAlerta };
        DWORD res = WaitForMultipleObjects(2, espNovoAlerta, FALSE, INFINITE);

        if (res == WAIT_OBJECT_0) break;         /* eventoParar */
        if (res != WAIT_OBJECT_0 + 1) continue; /* eventoNovoAlerta (auto-reset) */

        /* Ler dados do alerta com lock */
        EnterCriticalSection(&ctx->csAlerta);
        MSG_ALERTA alerta = ctx->alertaAtivo;
        LeaveCriticalSection(&ctx->csAlerta);

        alerta.msg[_countof(alerta.msg) - 1] = _T('\0');
        ImprimirComTimestamp(ctx, alerta.msg);

        /* Criar waitable timer para a duração */
        HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
        if (timer == NULL) {
            PrintConsola(ctx, _T("Aviso: CreateWaitableTimer falhou (%lu)\n"), GetLastError());
            continue;
        }
        LARGE_INTEGER li;
        li.QuadPart = -((LONGLONG)alerta.duracao * 10000000LL);
        SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);

        /* Aguardar: parar / novo alerta / cancelar / timer expirou */
        HANDLE espDuracao[4] = {
            ctx->eventoParar,
            ctx->eventoNovoAlerta,
            ctx->eventoCancelarAlerta,
            timer
        };
        DWORD resDuracao = WaitForMultipleObjects(4, espDuracao, FALSE, INFINITE);
        CloseHandle(timer);

        if (resDuracao == WAIT_OBJECT_0) {
            /* eventoParar */
            break;
        } else if (resDuracao == WAIT_OBJECT_0 + 1) {
            /* Novo alerta — loop volta ao início (eventoNovoAlerta já consumido) */
            /* Mostrar o novo alerta na próxima iteração */
            /* Mas precisamos de o processar agora — re-sinalizar para o próximo ciclo */
            /* Como é auto-reset e já foi consumido pelo WaitForMultipleObjects,
               precisamos de o re-sinalizar para que o loop externo o apanhe */
            SetEvent(ctx->eventoNovoAlerta);
            continue;
        } else if (resDuracao == WAIT_OBJECT_0 + 2) {
            /* Alerta cancelado pelo central */
            ImprimirComTimestamp(ctx, _T("---"));
            /* Não enviar tipo 3 — foi o central que cancelou */
        } else if (resDuracao == WAIT_OBJECT_0 + 3) {
            /* Timer expirou — alerta terminou naturalmente */
            ImprimirComTimestamp(ctx, _T("---"));
            /* Notificar o central: fim do alerta (tipo 3) */
            MSG_CMD cmd3;
            cmd3.tipo = 3;
            EscreverPipe(ctx, &cmd3, sizeof(MSG_CMD));
            EnterCriticalSection(&ctx->csAlerta);
            ctx->temAlerta = 0;
            LeaveCriticalSection(&ctx->csAlerta);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread que recebe mensagens do central (única thread a ler do pipe) */
/* ------------------------------------------------------------------ */
static DWORD WINAPI ThreadReceberCentral(LPVOID param) {
    CONTEXTO_APP* ctx = (CONTEXTO_APP*)param;

    /* Buffer grande o suficiente para a maior mensagem possível */
    BYTE buf[sizeof(MSG_ALERTA) + 32];

    for (;;) {
        if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;

        DWORD lidos = 0;
        /* Leitura bloqueante — sem lock pois é a única thread a ler */
        BOOL ok = ReadFile(ctx->hPipe, buf, sizeof(buf), &lidos, NULL);

        if (!ok || lidos == 0) {
            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
                PrintConsola(ctx, _T("[Placar] Ligacao ao central perdida.\n"));
                InterlockedExchange(&ctx->deveSair, 1);
                SetEvent(ctx->eventoParar);
                /* Desbloquear thread de comandos se estiver à espera */
                SetEvent(ctx->eventoRespostaLigar);
                SetEvent(ctx->eventoRespostaDesligar);
            }
            break;
        }

        BYTE tipo = buf[0];

        if (tipo == 4) {
            /* Novo alerta */
            if (lidos < sizeof(MSG_ALERTA)) {
                PrintConsola(ctx, _T("[Placar] Mensagem de alerta incompleta (%lu bytes).\n"), lidos);
                continue;
            }
            MSG_ALERTA* alerta = (MSG_ALERTA*)buf;
            alerta->msg[_countof(alerta->msg) - 1] = _T('\0');

            /* Guardar alerta ativo */
            EnterCriticalSection(&ctx->csAlerta);
            ctx->alertaAtivo = *alerta;
            ctx->temAlerta = 1;
            LeaveCriticalSection(&ctx->csAlerta);

            /* Confirmar receção ao central (enviar a mesma estrutura de volta) */
            EscreverPipe(ctx, alerta, sizeof(MSG_ALERTA));

            /* Sinalizar thread de alertas (auto-reset) */
            SetEvent(ctx->eventoNovoAlerta);

        } else if (tipo == 5) {
            /* Cancelar alerta */
            /* Confirmar ao central */
            MSG_CMD conf;
            conf.tipo = 5;
            EscreverPipe(ctx, &conf, sizeof(MSG_CMD));

            EnterCriticalSection(&ctx->csAlerta);
            ctx->temAlerta = 0;
            LeaveCriticalSection(&ctx->csAlerta);

            /* Sinalizar thread de alertas para cancelar o timer */
            SetEvent(ctx->eventoCancelarAlerta);

        } else if (tipo == 6) {
            /* Encerrar plataforma */
            PrintConsola(ctx, _T("[Placar] Central encerrou a plataforma. A terminar...\n"));
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            /* Desbloquear thread de comandos se estiver à espera */
            SetEvent(ctx->eventoRespostaLigar);
            SetEvent(ctx->eventoRespostaDesligar);
            break;

        } else if (tipo == 7) {
            /* Resposta ao ligar: identificador atribuído */
            if (lidos >= sizeof(MSG_ID)) {
                MSG_ID* mid = (MSG_ID*)buf;
                ctx->identificador = mid->identificador;
            }
            /* Sinalizar thread de comandos */
            SetEvent(ctx->eventoRespostaLigar);

        } else if (tipo == 2) {
            /* Confirmação de desligar */
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

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        EnterCriticalSection(&ctx->csConsola);
        _tprintf(_T("CMD> "));
        LeaveCriticalSection(&ctx->csConsola);

        if (_fgetts(comando, TAM_MAX_COMANDO, stdin) == NULL) {
            InterlockedExchange(&ctx->deveSair, 1);
            SetEvent(ctx->eventoParar);
            break;
        }

        size_t tam = _tcslen(comando);
        while (tam > 0 && (comando[tam - 1] == _T('\n') || comando[tam - 1] == _T('\r')))
            comando[--tam] = _T('\0');

        if (tam == 0) continue;

        if (_tcsicmp(comando, _T("liga")) == 0) {
            if (InterlockedCompareExchange(&ctx->ligado, 0, 0) != 0) {
                PrintConsola(ctx, _T("Placar ja se encontra ligado.\n"));
                continue;
            }

            /* Enviar pedido de ligar ao central (MSG_CMD tipo 1) */
            MSG_CMD cmd;
            cmd.tipo = 1;
            if (!EscreverPipe(ctx, &cmd, sizeof(MSG_CMD))) {
                PrintConsola(ctx, _T("[Placar] Erro ao enviar pedido de ligar ao central.\n"));
                continue;
            }

            /* Aguardar resposta da ThreadReceberCentral (evento eventoRespostaLigar) */
            HANDLE espLigar[2] = { ctx->eventoParar, ctx->eventoRespostaLigar };
            DWORD res = WaitForMultipleObjects(2, espLigar, FALSE, 5000);

            if (res != WAIT_OBJECT_0 + 1) {
                PrintConsola(ctx, _T("[Placar] Timeout ou erro a aguardar resposta do central.\n"));
                continue;
            }

            if (InterlockedCompareExchange(&ctx->deveSair, 0, 0) != 0) break;

            InterlockedExchange(&ctx->ligado, 1);
            PrintConsola(ctx, _T("Identificador = %lu\n"), ctx->identificador);

        } else if (_tcsicmp(comando, _T("desliga")) == 0) {
            if (InterlockedCompareExchange(&ctx->ligado, 0, 0) == 0) {
                /* Não está ligado ao central — terminar diretamente */
                PrintConsola(ctx, _T("A terminar...\n"));
                InterlockedExchange(&ctx->deveSair, 1);
                SetEvent(ctx->eventoParar);
                break;
            }

            /* Enviar pedido de desligar ao central (MSG_CMD tipo 2) */
            MSG_CMD cmd;
            cmd.tipo = 2;
            EscreverPipe(ctx, &cmd, sizeof(MSG_CMD));

            /* Aguardar confirmação da ThreadReceberCentral */
            HANDLE espDesligar[2] = { ctx->eventoParar, ctx->eventoRespostaDesligar };
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
    ZeroMemory(&ctx, sizeof(ctx));

    InitializeCriticalSection(&ctx.csConsola);
    InitializeCriticalSection(&ctx.csPipe);
    InitializeCriticalSection(&ctx.csAlerta);

    /* Obter nome do pipe (linha de comandos ou registry) */
    const TCHAR* argPipe = (argc >= 2 && argv[1] != NULL && argv[1][0] != _T('\0'))
                           ? argv[1] : NULL;

    if (!GarantirNomePipeNoRegisto(argPipe, ctx.nomePipe, _countof(ctx.nomePipe))) {
        DeleteCriticalSection(&ctx.csConsola);
        DeleteCriticalSection(&ctx.csPipe);
        DeleteCriticalSection(&ctx.csAlerta);
        return 1;
    }

    PrintConsola(&ctx, _T("NamedPipe = '%s'\n"), ctx.nomePipe);

    /* Construir nome completo do pipe */
    TCHAR nomePipeCompleto[TAM_NOME_PIPE + 16];
    _sntprintf(nomePipeCompleto, _countof(nomePipeCompleto),
               _T("%s%s"), PREFIXO_PIPE, ctx.nomePipe);

    /* Ligar ao central via named pipe (com retry) */
    PrintConsola(&ctx, _T("[Placar] A ligar ao central em '%s'...\n"), nomePipeCompleto);

    BOOL ligadoPipe = FALSE;
    for (int tentativa = 0; tentativa < 5; tentativa++) {
        ctx.hPipe = CreateFile(
            nomePipeCompleto,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL,
            OPEN_EXISTING,
            0, NULL
        );
        if (ctx.hPipe != INVALID_HANDLE_VALUE) {
            ligadoPipe = TRUE;
            break;
        }
        DWORD err = GetLastError();
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

    /* Configurar modo message no lado cliente */
    DWORD modoLeitura = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(ctx.hPipe, &modoLeitura, NULL, NULL);

    PrintConsola(&ctx, _T("[Placar] Ligado ao central. Use 'liga' para registar o placar.\n"));

    /* Criar eventos */
    ctx.eventoParar           = CreateEvent(NULL, TRUE,  FALSE, NULL); /* manual-reset */
    ctx.eventoNovoAlerta      = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */
    ctx.eventoCancelarAlerta  = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */
    ctx.eventoRespostaLigar   = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */
    ctx.eventoRespostaDesligar= CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */

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

    /* Criar threads */
    HANDLE hThreadCmds    = CreateThread(NULL, 0, ThreadComandos,       &ctx, 0, NULL);
    HANDLE hThreadAlertas = CreateThread(NULL, 0, ThreadAlertas,        &ctx, 0, NULL);
    HANDLE hThreadCentral = CreateThread(NULL, 0, ThreadReceberCentral, &ctx, 0, NULL);

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

    /* Aguardar todas as threads */
    HANDLE threads[3] = { hThreadCmds, hThreadAlertas, hThreadCentral };
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
