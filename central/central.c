#include "../protocologo.h"
#include <stdio.h>
#include <tchar.h>

int _tmain(int argc, TCHAR* argv[]) {
    HANDLE hPipe;
    TCHAR nomePipe[256];
    BOOL ligado;
    DWORD escritos, lidos;

    if (argc < 2) {
        _tprintf(_T("Uso: central.exe <nome_pipe>\n"));
        return 1;
    }

    _stprintf_s(nomePipe, 256, _T("\\\\.\\pipe\\%s"), argv[1]);
    _tprintf(_T("[CENTRAL] A criar o pipe: %s\n"), nomePipe);

    hPipe = CreateNamedPipe(
        nomePipe,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        MAX_PLACAR,
        sizeof(MSG_ALERTA),
        sizeof(MSG_ALERTA),
        0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[ERRO] Falha ao criar pipe (%lu)\n"), GetLastError());
        return 1;
    }

    _tprintf(_T("[CENTRAL] Aguardando que um placar se ligue...\n"));

    ligado = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

    if (ligado) {
        _tprintf(_T("[CENTRAL] Placar ligado com sucesso!\n"));

        
        MSG_CMD msgRecebida;
        if (ReadFile(hPipe, &msgRecebida, sizeof(MSG_CMD), &lidos, NULL)) {
            if (msgRecebida.tipo == TIPO_LIGAR) {
                _tprintf(_T("[CENTRAL] Recebido pedido para LIGAR.\n"));

               
                MSG_ID resposta;
                resposta.tipo = TIPO_RESPOSTA_ID;
                resposta.identificador = 123;
                WriteFile(hPipe, &resposta, sizeof(MSG_ID), &escritos, NULL);
                _tprintf(_T("[CENTRAL] ID 123 enviado ao placar.\n"));

               

                _tprintf(_T("[CENTRAL] A preparar envio de alerta...\n"));
                Sleep(1000); 

                MSG_ALERTA alerta;
                alerta.tipo = TIPO_NOVO_ALERTA;
                _tcscpy_s(alerta.msg, 140, _T("ALERTA DE TESTE: Golo no Estadio!"));
                alerta.duracao = 7; 
                if (WriteFile(hPipe, &alerta, sizeof(MSG_ALERTA), &escritos, NULL)) {
                    _tprintf(_T("[CENTRAL] Alerta enviado. A aguardar que o Placar termine...\n"));

                    
                    MSG_CMD confirmacao;
                    if (ReadFile(hPipe, &confirmacao, sizeof(MSG_CMD), &lidos, NULL)) {
                        if (confirmacao.tipo == TIPO_FIM_ALERTA) {
                            _tprintf(_T("[CENTRAL] O Placar confirmou que o alerta terminou. Sucesso!\n"));
                        }
                    }
                }
            }
        }
    }

    _tprintf(_T("[CENTRAL] A fechar...\n"));
    CloseHandle(hPipe);
    return 0;
}