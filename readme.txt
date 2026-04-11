SO2 - Meta 1 - placar
---------------------

Programa em C (Win32) para a primeira entrega do trabalho lab.

Resumindo o que faz:
- Arranca com o nome do pipe na linha de comandos (placar.exe <nome>) ou le o NPIPE em
  HKCU\Software\TrabSO2; se nao houver nada dos dois da erro e sai.
- Se passare o nome na linha de comandos, grava/atualiza o NPIPE no registo.
- Mostra "Named Pipe = '...'" ao inicio.
- Comandos na consola: "liga" (da um ID aleatorio entre 1 e 999) e "desliga".
- Ha duas threads: uma le comandos, outra fica a espera do evento "notificar", le o alerta
  em REG_BINARY (valor com o mesmo nome do pipe, struct MSG_ALERTA tipo 4) e usa um timer
  para mostrar '---' quando acaba o tempo. Se chegar outro alerta a meio, passa a contar
  o novo e esquece o timer antigo.

Para compilar (prompt do VS, x64):
  cl /DUNICODE /D_UNICODE /W4 /EHsc placar.c /Fe:placar.exe /link advapi32.lib