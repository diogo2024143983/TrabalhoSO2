# Trabalho SO2 - M1 (base)

Base inicial do programa `placar` para a etapa 1.

## O que ja esta implementado

- Resolucao do nome do named pipe:
  - pela linha de comandos (`placar.exe <nome_pipe>`) ou
  - pelo valor `NPIPE` em `HKEY_CURRENT_USER\Software\TrabSO2`.
- Criacao/acesso da chave `Software\TrabSO2` no Registry.
- Gravacao do `NPIPE` no Registry quando fornecido na linha de comandos.
- Impressao do nome do pipe na consola:
  - `NamedPipe = 'tubo'`
- Duas threads:
  - thread de comandos de utilizador (`ligar`, `desligar`);
  - thread de "rececao" de alertas.
- Evento nomeado de reset manual `notificar`.
- Simulacao de rececao de alerta:
  - espera no evento `notificar`;
  - leitura de `REG_BINARY` no valor com nome igual ao pipe (ex.: `tubo`);
  - estrutura esperada: `MSG_ALERTA` (`tipo=4`, `msg`, `duracao`).
- Apresentacao do alerta com timestamp.
- Temporizacao com `waitable timer` e apresentacao de `---` apos terminar a duracao.
- Terminacao controlada com libertacao de recursos.

## Compilar (exemplo)

Developer Command Prompt for VS:

```bat
cl /DUNICODE /D_UNICODE /W4 /EHsc placar.c /link advapi32.lib
```

## O que falta para fechar a M1

1. Testador auxiliar para disparar alertas:
   - escrever `REG_BINARY` com `MSG_ALERTA` no valor `<nome_pipe>`;
   - fazer `SetEvent("notificar")`.
2. Ajustar parser de linha de comandos para formato final pretendido pelo grupo
   (ex.: `-p <pipe>` em vez de argumento posicional).
3. Validar com testes de demonstracao:
   - comandos simultaneos enquanto alertas chegam;
   - multiplos alertas seguidos;
   - `desligar` durante alerta ativo.
4. Confirmar exatamente os textos exigidos pelo docente (formatos de output).
5. Opcional recomendado para robustez:
   - timeout/retentativa na leitura do Registry;
   - validacao adicional de `duracao` e string.

## Nota

Esta base implementa a simulacao pedida para a 1a meta.  
A comunicacao real por named pipes com o `central` fica para a entrega seguinte (M2), conforme enunciado.
