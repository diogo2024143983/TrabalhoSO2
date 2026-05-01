#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../protocologo.h"

#define TAM_MAX_COMANDO 256

typedef struct {
    HANDLE hPipe;
    HANDLE eventoParar;
    CRITICAL_SECTION csEscrita;
    DWORD idAtribuido;
    volatile LONG deveSair;
} CONTEXTO_APP;

// Função auxiliar para prints organizados
static void ImprimirComTimestamp(CONTEXTO_APP* contexto, const TCHAR* texto) {
    SYSTEMTIME ts;
    GetLocalTime(&ts);
    EnterCriticalSection(&contexto->csEscrita);
    _tprintf(_T("%02u:%02u:%02u: %s\n"), ts.wHour, ts.wMinute, ts.wSecond, texto);
    LeaveCriticalSection(&contexto->csEscrita);
}

// Thread que fica à espera de alertas vindos da Central via Pipe
static DWORD WINAPI ThreadAlertas(LPVOID parametro) {
    CONTEXTO_APP* contexto = (CONTEXTO_APP*)parametro;
    MSG_ALERTA alerta;
    DWORD lidos, escritos;

    while (InterlockedCompareExchange(&contexto->deveSair, 0, 0) == 0) {
        // aqui fica bloqueado aqui até a Central enviar algo
        if (!ReadFile(contexto->hPipe, &alerta, sizeof(MSG_ALERTA), &lidos, NULL)) {
            ImprimirComTimestamp(contexto, _T("Conexao com a Central perdida."));
            break;
        }

        if (alerta.tipo == TIPO_NOVO_ALERTA) {
            ImprimirComTimestamp(contexto, alerta.msg);

           
            Sleep(alerta.duracao * 1000);

           
            MSG_CMD msgFim = { TIPO_FIM_ALERTA };
            WriteFile(contexto->hPipe, &msgFim, sizeof(MSG_CMD), &escritos, NULL);

            ImprimirComTimestamp(contexto, _T("--- (Alerta Terminado) ---"));
        }
    }
    return 0;
}

// Thread para comandos do utilizador 
static DWORD WINAPI ThreadComandos(LPVOID parametro) {
    CONTEXTO_APP* contexto = (CONTEXTO_APP*)parametro;
    TCHAR comando[TAM_MAX_COMANDO];

    while (InterlockedCompareExchange(&contexto->deveSair, 0, 0) == 0) {
        _tprintf(_T("CMD> "));
        if (_fgetts(comando, TAM_MAX_COMANDO, stdin) == NULL) break;

        
        comando[_tcslen(comando) - 1] = _T('\0');

        if (_tcsicmp(comando, _T("desliga")) == 0) {
            ImprimirComTimestamp(contexto, _T("A desligar o placar..."));
            InterlockedExchange(&contexto->deveSair, 1);
            break;
        }
    }
    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
    CONTEXTO_APP contexto;
    ZeroMemory(&contexto, sizeof(contexto));
    InitializeCriticalSection(&contexto.csEscrita);

    if (argc < 2) {
        _tprintf(_T("Uso: placar.exe <nome_pipe>\n"));
        return 1;
    }

    TCHAR fullPipeName[256];
    _stprintf_s(fullPipeName, 256, _T("\\\\.\\pipe\\%s"), argv[1]);

    _tprintf(_T("A tentar ligar a Central em: %s\n"), fullPipeName);

   
    contexto.hPipe = CreateFile(fullPipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (contexto.hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(_T("Erro: Central nao encontrada! (Corre a Central primeiro)\n"));
        DeleteCriticalSection(&contexto.csEscrita);
        return 1;
    }

    
    MSG_CMD cmdLigar = { TIPO_LIGAR };
    DWORD escritos, lidos;
    WriteFile(contexto.hPipe, &cmdLigar, sizeof(MSG_CMD), &escritos, NULL);

    MSG_ID respostaId;
    if (ReadFile(contexto.hPipe, &respostaId, sizeof(MSG_ID), &lidos, NULL)) {
        contexto.idAtribuido = respostaId.identificador;
        _tprintf(_T("Sucesso! O meu ID e: %lu\n"), contexto.idAtribuido);
    }

    
    HANDLE threads[2];
    threads[0] = CreateThread(NULL, 0, ThreadAlertas, &contexto, 0, NULL);
    threads[1] = CreateThread(NULL, 0, ThreadComandos, &contexto, 0, NULL);

   
    WaitForMultipleObjects(2, threads, FALSE, INFINITE);

    
    CloseHandle(contexto.hPipe);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    DeleteCriticalSection(&contexto.csEscrita);

    return 0;
}