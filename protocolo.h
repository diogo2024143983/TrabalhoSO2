#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <windows.h>
#include <tchar.h>

#define MAX_PLACAR      16
#define TAM_MSG         256

// Message types
#define TIPO_LIGAR          1
#define TIPO_DESLIGAR       2
#define TIPO_FIM_ALERTA     3
#define TIPO_NOVO_ALERTA    4
#define TIPO_CANCELAR       5
#define TIPO_ENCERRAR       6
#define TIPO_RESPOSTA_ID    7

// Message structures
typedef struct {
    BYTE tipo;
} MSG_CMD;

typedef struct {
    BYTE tipo;
    DWORD duracao;
    TCHAR msg[TAM_MSG];
} MSG_ALERTA;

typedef struct {
    BYTE tipo;
    DWORD identificador;
} MSG_ID;

typedef struct {
    DWORD identificador;
    DWORD duracao;
    TCHAR msg[140];
    DWORD placares[20];
} ALERTA_INFO;

typedef struct {
    ALERTA_INFO alertas[20];
    int num_alertas;
    BOOL desligar;
} SHM_ALERTA;

#endif