#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winsock2.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../protocolo.h"

#pragma comment(lib, "ws2_32.lib")

#define TAM_SHARED_MEM   65536
#define PORTA_MONITOR    5555
#define MAX_CLIENTES     256

typedef struct {
    BOOL ativo;
    DWORD identificador;
    BOOL temAlerta;
    TCHAR msgAlerta[TAM_MSG];
} ESTADO_PLACAR_MONITOR;

typedef struct {
    ESTADO_PLACAR_MONITOR placares[MAX_PLACAR];
    DWORD numPlacares;
    SYSTEMTIME ultimaAtualizacao;
} DADOS_MONITOR;

typedef struct {
    HANDLE hMapFile;
    DADOS_MONITOR* pDados;
    SOCKET sockUDP;
    SOCKET sockTCP;
    HANDLE eventoParar;
    CRITICAL_SECTION csConsola;
    volatile LONG deveSair;
} CONTEXTO_MONITOR;

static void PrintConsola(CONTEXTO_MONITOR* ctx, const TCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EnterCriticalSection(&ctx->csConsola);
    _vtprintf(fmt, args);
    LeaveCriticalSection(&ctx->csConsola);
    va_end(args);
}

static BOOL InicializarSharedMemory(CONTEXTO_MONITOR* ctx) {
    ctx->hMapFile = OpenFileMapping(FILE_MAP_READ, FALSE, _T("TrabSO2_SharedMem"));
    if (ctx->hMapFile == NULL) {
        PrintConsola(ctx, _T("[Monitor] Criando shared memory...\n"));
        ctx->hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, TAM_SHARED_MEM, _T("TrabSO2_SharedMem"));
        if (ctx->hMapFile == NULL) {
            PrintConsola(ctx, _T("[Monitor] Erro ao criar shared memory\n"));
            return FALSE;
        }
    }

    ctx->pDados = (DADOS_MONITOR*)MapViewOfFile(ctx->hMapFile, FILE_MAP_READ, 0, 0, TAM_SHARED_MEM);
    if (ctx->pDados == NULL) {
        PrintConsola(ctx, _T("[Monitor] Erro ao mapear shared memory\n"));
        CloseHandle(ctx->hMapFile);
        return FALSE;
    }

    PrintConsola(ctx, _T("[Monitor] Shared memory conectada com sucesso\n"));
    return TRUE;
}

static BOOL InicializarSockets(CONTEXTO_MONITOR* ctx) {
    WSADATA wsaData;
    struct sockaddr_in addr;
    int reuso = 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        PrintConsola(ctx, _T("[Monitor] Erro ao inicializar Winsock\n"));
        return FALSE;
    }

    // Socket UDP para broadcast
    ctx->sockUDP = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->sockUDP == INVALID_SOCKET) {
        PrintConsola(ctx, _T("[Monitor] Erro ao criar socket UDP\n"));
        WSACleanup();
        return FALSE;
    }

    setsockopt(ctx->sockUDP, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuso, sizeof(reuso));
    
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    addr.sin_port = htons(PORTA_MONITOR);

    if (bind(ctx->sockUDP, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        PrintConsola(ctx, _T("[Monitor] Erro ao fazer bind UDP\n"));
        closesocket(ctx->sockUDP);
        WSACleanup();
        return FALSE;
    }

    int broadcast = 1;
    setsockopt(ctx->sockUDP, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    // Socket TCP para clientes
    ctx->sockTCP = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ctx->sockTCP == INVALID_SOCKET) {
        PrintConsola(ctx, _T("[Monitor] Erro ao criar socket TCP\n"));
        closesocket(ctx->sockUDP);
        WSACleanup();
        return FALSE;
    }

    setsockopt(ctx->sockTCP, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuso, sizeof(reuso));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORTA_MONITOR + 1);

    if (bind(ctx->sockTCP, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        PrintConsola(ctx, _T("[Monitor] Erro ao fazer bind TCP\n"));
        closesocket(ctx->sockUDP);
        closesocket(ctx->sockTCP);
        WSACleanup();
        return FALSE;
    }

    if (listen(ctx->sockTCP, SOMAXCONN) == SOCKET_ERROR) {
        PrintConsola(ctx, _T("[Monitor] Erro ao fazer listen TCP\n"));
        closesocket(ctx->sockUDP);
        closesocket(ctx->sockTCP);
        WSACleanup();
        return FALSE;
    }

    PrintConsola(ctx, _T("[Monitor] Sockets criados com sucesso (UDP:%d, TCP:%d)\n"), PORTA_MONITOR, PORTA_MONITOR + 1);
    return TRUE;
}

static char* Convertir(char* buffer, int buflen, const TCHAR* texto) {
#ifdef _UNICODE
    int len = WideCharToMultiByte(CP_ACP, 0, texto, -1, buffer, buflen, NULL, NULL);
    if (len == 0) buffer[0] = '\0';
#else
    strncpy_s(buffer, buflen, texto, _TRUNCATE);
#endif
    return buffer;
}

static void EnviarEstado(CONTEXTO_MONITOR* ctx) {
    char buffer[2048];
    int pos = 0;
    int i;
    char temp[512];
    struct sockaddr_in broadcast_addr;

    if (ctx->pDados == NULL) return;

    pos += sprintf_s(buffer + pos, sizeof(buffer) - pos, "=== PLACAR INFORMATIVO ===\n");
    pos += sprintf_s(buffer + pos, sizeof(buffer) - pos, "Placares ativos: %lu\n", ctx->pDados->numPlacares);

    for (i = 0; i < MAX_PLACAR; i++) {
        if (!ctx->pDados->placares[i].ativo) continue;
        
        Convertir(temp, sizeof(temp), ctx->pDados->placares[i].msgAlerta);
        if (ctx->pDados->placares[i].temAlerta) {
            pos += sprintf_s(buffer + pos, sizeof(buffer) - pos, 
                "  Placar %lu: %s\n", 
                ctx->pDados->placares[i].identificador,
                temp);
        } else {
            pos += sprintf_s(buffer + pos, sizeof(buffer) - pos, 
                "  Placar %lu: sem alerta\n", 
                ctx->pDados->placares[i].identificador);
        }
    }

    // Enviar via UDP broadcast
    ZeroMemory(&broadcast_addr, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcast_addr.sin_port = htons(PORTA_MONITOR);

    sendto(ctx->sockUDP, buffer, (int)strlen(buffer), 0, (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
}

static DWORD WINAPI ThreadAtualizacao(LPVOID param) {
    CONTEXTO_MONITOR* ctx = (CONTEXTO_MONITOR*)param;

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        EnviarEstado(ctx);
        Sleep(1000);
    }
    return 0;
}

static DWORD WINAPI ThreadClienteTCP(LPVOID param) {
    SOCKET clientSocket = (SOCKET)param;
    char buffer[2048];
    int recebidos;

    while (1) {
        recebidos = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (recebidos <= 0) break;
        buffer[recebidos] = '\0';

        if (strncmp(buffer, "STATUS", 6) == 0) {
            // Cliente pediu status - será enviado pela thread de atualização
            continue;
        }
    }

    closesocket(clientSocket);
    return 0;
}

static DWORD WINAPI ThreadAceitarClientes(LPVOID param) {
    CONTEXTO_MONITOR* ctx = (CONTEXTO_MONITOR*)param;
    SOCKET clientSocket;
    struct sockaddr_in clientAddr;
    int addrLen = sizeof(clientAddr);
    HANDLE hThread;

    while (InterlockedCompareExchange(&ctx->deveSair, 0, 0) == 0) {
        clientSocket = accept(ctx->sockTCP, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }

        hThread = CreateThread(NULL, 0, ThreadClienteTCP, (LPVOID)clientSocket, 0, NULL);
        if (hThread != NULL) {
            CloseHandle(hThread);
        } else {
            closesocket(clientSocket);
        }
    }
    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
    CONTEXTO_MONITOR ctx;
    HANDLE hThreadAtualizacao, hThreadClientes;

    ZeroMemory(&ctx, sizeof(ctx));
    InitializeCriticalSection(&ctx.csConsola);
    ctx.eventoParar = CreateEvent(NULL, TRUE, FALSE, NULL);

    PrintConsola(&ctx, _T("[Monitor] Iniciando Monitor da Plataforma Placar...\n"));

    if (!InicializarSharedMemory(&ctx)) {
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    if (!InicializarSockets(&ctx)) {
        UnmapViewOfFile(ctx.pDados);
        CloseHandle(ctx.hMapFile);
        DeleteCriticalSection(&ctx.csConsola);
        return 1;
    }

    hThreadAtualizacao = CreateThread(NULL, 0, ThreadAtualizacao, &ctx, 0, NULL);
    hThreadClientes = CreateThread(NULL, 0, ThreadAceitarClientes, &ctx, 0, NULL);

    if (!hThreadAtualizacao || !hThreadClientes) {
        InterlockedExchange(&ctx.deveSair, 1);
        SetEvent(ctx.eventoParar);
        return 1;
    }

    PrintConsola(&ctx, _T("[Monitor] Aguardando pressionar ENTER para sair...\n"));
    _getts_s(NULL, 0);

    InterlockedExchange(&ctx.deveSair, 1);
    SetEvent(ctx.eventoParar);

    WaitForSingleObject(hThreadAtualizacao, 3000);
    WaitForSingleObject(hThreadClientes, 3000);

    closesocket(ctx.sockUDP);
    closesocket(ctx.sockTCP);
    WSACleanup();

    UnmapViewOfFile(ctx.pDados);
    CloseHandle(ctx.hMapFile);
    CloseHandle(hThreadAtualizacao);
    CloseHandle(hThreadClientes);
    CloseHandle(ctx.eventoParar);
    DeleteCriticalSection(&ctx.csConsola);

    return 0;
}
