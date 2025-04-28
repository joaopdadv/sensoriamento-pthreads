#!/usr/bin/env bash

SRC="main.c"
OUT_DIR="output"
OUT_BIN="$OUT_DIR/main.out"

function error_exit {
  echo "Erro: $1" >&2
  exit 1
}

# Verifica se o arquivo-fonte existe
if [[ ! -f "$SRC" ]]; then
  error_exit "Arquivo '$SRC' não encontrado."
fi

# Cria o diretório de saída se não existir
mkdir -p "$OUT_DIR" || error_exit "Não foi possível criar o diretório '$OUT_DIR'."

echo "Compilando $SRC -> $OUT_BIN"
gcc "$SRC" -o "$OUT_BIN" || error_exit "Falha na compilação."

echo "Executando $OUT_BIN"
"$OUT_BIN"
