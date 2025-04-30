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
