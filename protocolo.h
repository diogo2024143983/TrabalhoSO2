#pragma once
#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <windows.h>
#include <tchar.h>


#define MAX_PLACAR 20
#define TAM_MSG    140


#define TIPO_LIGAR       1  
#define TIPO_DESLIGAR    2  
#define TIPO_FIM_ALERTA  3  
#define TIPO_NOVO_ALERTA 4  
#define TIPO_CANCELAR    5  
#define TIPO_ENCERRAR    6  
#define TIPO_RESPOSTA_ID 7  



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

#endif
