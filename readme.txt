SO2 - Trabalho Laboratorial - M3
=================================
Identificacao do Grupo
Diogo Ribeiro Costa - 2024143983
Rodrigo Cravo Pereira - 2024117439

Declaracao de Defesa Oral
Nao pretendemos realizar a defesa oral, individual e presencial, do trabalho laboratorial. Submetemos o projeto exclusivamente para a avaliacao funcional base (2.5 valores). O relatorio detalhado foi, por isso, omitido da submissao.

Estrutura do projeto
TrabalhoSO2.sln        - Solucao Visual Studio 2022
central\               - Codigo fonte e projeto do programa central
monitor\               - Codigo fonte e projeto do programa monitor (Win32 GUI)
protocolo.h            - Estruturas de dados, Named Pipes e Memoria Partilhada
readme.txt             - Este ficheiro

Compilacao
Abrir TrabalhoSO2.sln no Visual Studio 2022.
Selecionar "Build" -> "Build Solution" (Ctrl+Shift+B).
Os executaveis serao compilados nas respetivas diretorias x64\Debug de cada projeto.

Requisitos Implementados na M3 (Entrega Final)
[x] Monitor desenvolvido com Interface Grafica nativa (API Win32).
[x] Comunicacao unidirecional Central -> Monitor estabelecida por Memoria Partilhada.
[x] Sincronizacao de acessos a Memoria Partilhada protegida rigorosamente por Mutex.
[x] Notificacao de atualizacoes baseada em Eventos do Windows (ausencia total de polling).
[x] DialogBox de Configuracao (definicao do limite de alertas por pagina e reconfiguracao dos nomes IPC).
[x] MessageBox "Acerca" com a identificacao dos autores.
[x] Paginacao do conteudo na interface atraves das teclas Page Up / Page Down e botoes na UI.
[x] Thread de timer na Central para atualizacao constante do tempo decrescente e injecao na SHM.
[x] Encerramento coordenado: a Central instrui a finalizacao do processo Monitor no fecho.

Requisitos Consolidados (M1 e M2)
[x] Comunicacao assincrona (Overlapped I/O) bidirecional por Named Pipes (Central <-> Placar).
[x] Central multiprocesso: suporta e comunica em tempo real com multiplas instancias do Placar em simultaneo.
[x] Tratamento de duracao de alertas locais garantido por Waitable Timers.
[x] Leitura estrita de identificadores e persistencia no Registry do Windows.
[x] Interfaces de controlo CLI por comandos assincronos na Central e no Placar.