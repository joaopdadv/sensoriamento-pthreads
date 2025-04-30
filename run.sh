#!/usr/bin/env bash

SRC="main.c"
OUT_DIR="output"
OUT_BIN="$OUT_DIR/main.out"
ZIP_FILE="devices_mqtt_data.zip"
EXTRACT_DIR="devices_mqtt_data"
CSV_FILE="devices.csv"

function error_exit {
  echo "Erro: $1" >&2
  exit 1
}

# Verifica se o arquivo-fonte existe
if [[ ! -f "$SRC" ]]; then
  error_exit "Arquivo '$SRC' não encontrado."
fi

# Gerencia extração de dados MQTT
if [[ -f "$EXTRACT_DIR/$CSV_FILE" ]]; then
  echo "Arquivo '$CSV_FILE' já existe em '$EXTRACT_DIR/'. Pulando extração."
elif [[ -f "$ZIP_FILE" ]]; then
  echo "Extraindo '$ZIP_FILE' para '$EXTRACT_DIR/'..."
  mkdir -p "$EXTRACT_DIR" || error_exit "Não foi possível criar o diretório '$EXTRACT_DIR'."
  unzip -o "$ZIP_FILE" -d "$EXTRACT_DIR" || error_exit "Falha ao extrair '$ZIP_FILE'."
else
  echo "Nenhum arquivo '$CSV_FILE' ou '$ZIP_FILE' encontrado. Pulando extração."
fi

# Cria o diretório de saída se não existir
mkdir -p "$OUT_DIR" || error_exit "Não foi possível criar o diretório '$OUT_DIR'."

echo "Compilando $SRC -> $OUT_BIN"
gcc "$SRC" -o "$OUT_BIN" || error_exit "Falha na compilação."

echo "Executando $OUT_BIN"
"$OUT_BIN"
