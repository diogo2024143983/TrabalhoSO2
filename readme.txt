SO2 - Trabalho Laboratorial - M2
=================================

Estrutura do projeto
--------------------
  TrabalhoSO2.sln        - Solucao Visual Studio 2022 (agrupa central e placar)
  central\
    central.c            - Codigo fonte do programa central
    central.vcxproj      - Projeto VS do central
  placar\
    placar.c             - Codigo fonte do programa placar
    placar.vcxproj       - Projeto VS do placar
  compilar.bat           - Script de compilacao rapida (MSBuild)
  readme.txt             - Este ficheiro

Compilacao
----------
  Opcao 1 - Abrir TrabalhoSO2.sln no Visual Studio 2022 e compilar (Ctrl+Shift+B)

  Opcao 2 - Executar compilar.bat (nao precisa de Developer Command Prompt):
    compilar.bat
    Os executaveis ficam em: x64\Debug\central.exe e x64\Debug\placar.exe

  Opcao 3 - Developer Command Prompt do VS2022 manualmente:
    cd central
    cl /W3 /TC /D_UNICODE /DUNICODE central.c /Fe:central.exe /link advapi32.lib
    cd ..\placar
    cl /W3 /TC /D_UNICODE /DUNICODE placar.c /Fe:placar.exe /link advapi32.lib

Utilizacao
----------
  1. Iniciar o central (numa consola):
       central.exe tubo
     O central fica a escutar no named pipe \\.\pipe\tubo

  2. Iniciar um ou mais placares (cada um numa consola separada):
       placar.exe tubo
     O placar liga-se ao central via \\.\pipe\tubo.
     Se o nome do pipe ja estiver guardado no Registry, pode omitir o argumento:
       placar.exe

  3. No placar, escrever:
       liga      -> regista o placar no central e recebe o identificador
       desliga   -> remove o placar da plataforma e termina

  4. No central, escrever:
       alerta <msg> <duracao_seg> <id_placar>
                 -> envia alerta ao placar indicado (id=0 envia a todos)
                 Exemplo: alerta A7 Norte - Acidente Km 34 300 1
       cancelar <id_placar>
                 -> cancela o alerta ativo no placar indicado
       listar    -> lista todos os placares ligados e alertas ativos
       encerrar  -> encerra a plataforma (notifica todos os placares)

Requisitos implementados - M2
------------------------------
Central:
  [x] 1. Recebe nome do pipe via linha de comandos (central.exe tubo -> \\.\pipe\tubo)
  [x] 2. Comando "alerta": envia MSG_ALERTA (tipo=4) ao(s) placar(es) e aguarda confirmacao
  [x] 2. Comando "cancelar": envia MSG_CMD (tipo=5) ao placar e aguarda confirmacao
  [x] 2. Comando "listar": mostra placares ligados e alertas ativos
  [x] 2. Comando "encerrar": envia MSG_CMD (tipo=6) a todos os placares
  [x] 3. Processa MSG_CMD tipo=1 (ligar): responde com MSG_ID (tipo=7)
  [x] 3. Processa MSG_CMD tipo=2 (desligar): responde com MSG_CMD (tipo=2)
  [x] 3. Processa MSG_CMD tipo=3 (fim alerta): atualiza estado interno
  [x] 4. Mantem estado dos placares, alertas ativos e identificadores

Placar (alteracoes M2):
  [x] 5. Comando "liga": envia MSG_CMD (tipo=1), recebe MSG_ID (tipo=7), mostra identificador
  [x] 6. Comando "desliga": envia MSG_CMD (tipo=2), recebe confirmacao, termina
  [x] 7. Timer expirado: envia MSG_CMD (tipo=3) ao central
  [x] 8. Recebe MSG_ALERTA (tipo=4): confirma com mesma estrutura, mostra na consola com timestamp
  [x] 9. Recebe MSG_CMD (tipo=5): confirma, mostra "---" com timestamp
  [x] 10. Recebe MSG_CMD (tipo=6): termina a execucao

Notas de implementacao
----------------------
- Named pipe bidirecional em modo MESSAGE (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE)
- O central cria uma thread por placar ligado (ThreadPlacar) que e a unica a ler do pipe
  desse placar, evitando race conditions nas leituras
- As confirmacoes de alerta/cancelar sao sinalizadas via eventos auto-reset da ThreadPlacar
  para a thread de comandos do administrador (sem ReadFile concorrente)
- O placar tem uma thread dedicada a receber mensagens do central (ThreadReceberCentral),
  que e a unica a ler do pipe, e comunica com as outras threads via eventos auto-reset
- Sincronizacao: CRITICAL_SECTION para escritas no pipe e para o array de placares;
  eventos auto-reset para sincronizacao entre threads
- O nome do pipe e guardado no Registry em HKCU\Software\TrabSO2\NPIPE (REG_SZ)
