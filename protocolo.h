#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <windows.h>
#include <tchar.h>

#define MAX_PLACAR      20
#define TAM_MSG         140

// Tipos de mensagem (MSG_CMD)
#define TIPO_LIGAR          1
#define TIPO_DESLIGAR       2
#define TIPO_FIM_ALERTA     3

// Tipos de mensagem (MSG_ALERTA)
#define TIPO_NOVO_ALERTA    4

// Tipos de mensagem (MSG_CMD do central para placar)
#define TIPO_CANCELAR       5
#define TIPO_ENCERRAR       6

// Tipos de mensagem (MSG_ID)
#define TIPO_RESPOSTA_ID    7

typedef struct {
    BYTE tipo;
} MSG_CMD;

typedef struct {
    BYTE tipo;
    TCHAR msg[TAM_MSG];
    DWORD duracao;
} MSG_ALERTA;

typedef struct {
    BYTE tipo;
    DWORD identificador;
} MSG_ID;

typedef struct {
    struct {
        DWORD identificador;
        DWORD duracao;
        TCHAR msg[TAM_MSG];
    } placar[MAX_PLACAR];
    BOOL desligar;
} SHM_ALERTA;

#endif