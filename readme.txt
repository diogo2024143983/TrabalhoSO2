SO2 - Trabalho Laboratorial - M3 (Final)
=========================================

Estrutura do projeto
--------------------
  TrabalhoSO2.sln        - Solucao Visual Studio 2022 (agrupa central, placar e monitor)
  protocolo.h            - Ficheiro de cabecalho partilhado (estruturas, definicoes)
  central\
    central.c            - Codigo fonte do programa central
    central.vcxproj      - Projeto VS do central
    central.vcxproj.user - Ficheiro de utilizador do VS
  placar\
    placar.c             - Codigo fonte do programa placar
    placar.vcxproj       - Projeto VS do placar
    placar.vcxproj.user  - Ficheiro de utilizador do VS
  monitor\
    monitor.c            - Codigo fonte do programa monitor (Win32 GUI)
    monitor.rc           - Recursos (dialogo de configuracao)
    resource.h           - Identificadores dos recursos
    monitor.vcxproj      - Projeto VS do monitor
    monitor.vcxproj.user - Ficheiro de utilizador do VS
  readme.txt             - Este ficheiro

Compilacao
----------
  Abrir TrabalhoSO2.sln no Visual Studio 2022 e compilar (Ctrl+Shift+B).
  Os executaveis ficam em x64\Debug\central.exe, x64\Debug\placar.exe e x64\Debug\monitor.exe

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
       ligar     -> regista o placar no central e recebe o identificador
       desligar  -> remove o placar da plataforma e termina

  4. No central, escrever:
       alerta <msg> <duracao_seg> <id_placar>
                 -> envia alerta ao placar indicado (id=0 envia a todos)
                 Exemplo: alerta "A7 Norte - Acidente Km 34" 300 1
       cancelar <id_placar>
                 -> cancela o alerta ativo no placar indicado
       listar    -> lista todos os placares ligados e alertas ativos
       encerrar  -> encerra a plataforma (notifica todos os placares)

  5. Iniciar o monitor (pode ter varias instancias):
       monitor.exe
     O monitor utiliza uma interface grafica Win32 para mostrar alertas ativos.
     Menu Ficheiro > Configuracao: permite definir numero maximo de alertas e nomes
       dos recursos de comunicacao (SHM, evento, mutex).
     Menu Ficheiro > Acerca: mostra informacao dos autores.
     Menu Ficheiro > Sair: termina o monitor.
     Page Up / Page Down: navegacao entre paginas de alertas.

Requisitos implementados - M3
-------------------------------
Central (alteracoes):
  [x] 5. Criacao de bloco de memoria partilhada (SHM_ALERTA) para partilha com monitor(es)
  [x] 6. Utilizacao de mutex para garantir coerencia da SHM
  [x] 6. Utilizacao de evento (auto-reset) para notificar monitores de alteracoes
  [x] 6. Nao utiliza polling - usa evento de notificacao (SetEvent)
  [x] Atualizacao da SHM sempre que: placar liga/desliga, alerta enviado/cancelado/expirado

Monitor:
  [x] 1. Menu "Ficheiro" com entradas "Configuracao", "Acerca" e "Sair"
  [x] 1. DialogBox de configuracao: numero maximo alertas e nomes dos recursos
  [x] 1. MessageBox "Acerca" com nome/numero dos autores
  [x] 1. Opcao "Sair" termina a aplicacao e liberta recursos
  [x] 2. ListView com identificador do placar, mensagem e duracao
  [x] 3. Page Up/Page Down para navegar entre paginas
  [x] SHM + evento de notificacao (sem polling) para detecao de alteracoes

Notas de implementacao
-----------------------
- Memoria partilhada: CreateFileMapping/MapViewOfFile (nome: Global\TrabSO2_ShmAlerta)
- Mutex: CreateMutex para acesso exclusivo a SHM (nome: Global\TrabSO2_MutexShm)
- Evento: CreateEvent para notificar monitores (nome: Global\TrabSO2_EvtUpdate)
- O central e os monitores usam os mesmos nomes de objetos (pre-definidos)
- A DialogBox do monitor permite reconfigurar os nomes (para flexibilidade)
- O monitor usa uma thread dedicada que espera no evento de notificacao
- Paginacao implementada na ListView com Page Up/Page Down