# Trabalho de análise de dados de sensoriamento utilizando pthreads - UCS

Este projeto compila e executa `main.c`, extraindo dados de `devices_mqtt_data.zip` se presente.

## Como usar

1. Coloque o arquivo `devices_mqtt_data.zip` na raiz do projeto (ao lado de `run.sh`), contendo dentro dele o arquivo `devices.csv`.
2. Torne o script executável (se necessário):
   ```bash
   chmod +x run.sh
   ```
3. Execute o script:
   ```bash
   ./run.sh
   ```

O script irá:

- Extrair `devices.csv` para a pasta `devices_mqtt_data/` caso ainda não exista.
- Compilar `main.c` e gerar o binário em `output/main.out`.
- Executar `output/main.out`.

## Estrutura de Arquivos

```
.
├── main.c
├── run.sh
├── devices_mqtt_data.zip
└── output/
```

## 1. Distribuição de carga entre threads

- Um thread **leitor** (`reader_thread`) faz o _enqueue_ de cada linha do CSV em uma fila compartilhada.
- Um conjunto de **threads de trabalho** (`worker_thread`) faz o _dequeue_ desta fila de forma dinâmica, processando cada linha conforme disponível. Cconsiderando um processador com n núcleos, são criadas (n - 1) threads de trabalho.
- **Critério de distribuição**: cada worker consome a próxima linha livre no buffer cridado pela thread leitora, sem divisão fixa por dispositivo ou período.

## 2. Análise dos dados pelas threads

- Cada worker, ao processar uma linha,:
  1. **Parseia** os campos (dispositivo, data e valores de sensores).
  2. Constrói uma chave (`device` + `ano-mês` + `sensor`).
  3. Chama `update_resultado()`, usando mutex:
     - Busca uma entrada existente em `results_array`.
     - Atualiza **mínimo**, **máximo**, **soma** e **contagem**.
     - Se não existir, **adiciona** nova entrada.
- O cálculo da média mensal é feito como `sum / count`.

## 3. Geração do arquivo CSV final

- Após todas as threads terminarem, `main()` chama `write_results()`:
  - Abre `resultados.csv`.
  - Escreve o **cabeçalho** (`device;ano-mes;sensor;valor_maximo;valor_medio;valor_minimo`).
  - Percorre `results_array`, formata `YYYY-MM` e escreve cada linha agregada.

## 4. Modo de execução das threads

- As threads são criadas via **POSIX threads** (pthreads), que correspondem a _kernel threads_ no Linux:
  - Elas executam em **espaço de usuário**, mas são agendadas pelo **kernel**.
  - Não há trocas de contexto usuário→núcleo a cada linha, apenas em chamadas a syscalls (ex.: E/S de arquivo ou sincronização de mutex).

## 5. Possíveis problemas de concorrência

- **Conteção de mutex**:
  - `queue_mtx` e `resultado_mutex` podem causar espera se muitos workers competirem pelo lock.
- **Sem ordenação garantida**:
  - A saída no CSV reflete a ordem de inserção, não há ordenação por dispositivo a menos que feita depois.
- **Buffering de E/S**:
  - `printf` das threads pode ser _buffered_, atrasando logs em stdout.
