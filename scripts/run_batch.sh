#!/usr/bin/env bash
# =============================================================================
# run_batch.sh — automatiza a simulação completa 
#
# O programa ns-3 (src/manet-routing.cc) já roda tudo sozinho quando
# executado (sem precisar de argumento nenhum). Este script só faz a ponte
# entre o ns-3 (onde o programa precisa estar, em $NS3_DIR/scratch/) e este
# projeto (onde o CSV de resultados deve ficar, em data/raw/):
#
#   1. liga src/manet-routing.cc a $NS3_DIR/scratch/ (symlink)
#   2. compila (./ns3 build)
#   3. roda (./ns3 run scratch/manet-routing) — isso sozinho já faz a
#      validação dos 3 protocolos e as 300 execuções da matriz experimental
#      (3 protocolos x 4 velocidades x 25 repetições)
#   4. copia o resultados.csv gerado de volta para data/raw/ deste projeto
#
# Uso:
#   export NS3_DIR=$HOME/ns-3-allinone      
#   bash scripts/run_batch.sh
#
# Se data/raw/resultados.csv já existir, é movido para um .bak antes de
# começar (evita sobrescrever/misturar resultados por engano).
#
# Com 25 repetições por combinação em vez de 5, a simulação demora bem mais
# (5x o tempo de execução). É normal o terminal ficar rodando por um tempo
# considerável antes de chegar em [300/300].
# =============================================================================
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${NS3_DIR:-}" ]]; then
  echo "ERRO: defina NS3_DIR (raiz do ns-3), ex.: export NS3_DIR=\$HOME/ns-3-allinone" >&2
  exit 1
fi
if [[ ! -x "$NS3_DIR/ns3" ]]; then
  echo "ERRO: nao encontrei o executavel '$NS3_DIR/ns3'. NS3_DIR esta correto?" >&2
  exit 1
fi

echo ">> Ligando src/manet-routing.cc a $NS3_DIR/scratch/ (symlink)"
# Link simbolico em vez de copia: o arquivo dentro do scratch/ do ns-3 passa a
# ser literalmente o mesmo arquivo do repositorio (nao uma copia desatualizavel).
# Beneficio extra: abrindo esse caminho no editor, o clangd acha o
# compile_commands.json que o ns-3 gera na sua propria raiz e resolve os
# includes ns3/*.h normalmente (editar direto em src/ de fora do ns-3 nao
# resolve, porque nao ha compile_commands.json ali -- isso e so um detalhe
# de editor/LSP, nao afeta a compilacao real, feita sempre pelo ./ns3 build).
rm -f "$NS3_DIR/scratch/manet-routing.cc"
ln -s "$PROJECT_DIR/src/manet-routing.cc" "$NS3_DIR/scratch/manet-routing.cc"

echo ">> Compilando (./ns3 build)..."
(cd "$NS3_DIR" && ./ns3 build)

echo ">> Rodando a simulacao (validacao dos 3 protocolos + 300 execucoes)."
echo ">> Com 25 repeticoes por combinacao isso demora bem mais que antes;"
echo ">> o progresso [n/300] aparece no terminal."
(cd "$NS3_DIR" && ./ns3 run scratch/manet-routing)

SRC_CSV="$NS3_DIR/resultado_simulacao/resultados.csv"
DST_CSV="$PROJECT_DIR/data/raw/resultados.csv"

if [[ ! -f "$SRC_CSV" ]]; then
  echo "ERRO: simulacao terminou mas nao encontrei $SRC_CSV" >&2
  exit 1
fi

mkdir -p "$PROJECT_DIR/data/raw"
if [[ -f "$DST_CSV" ]]; then
  mv "$DST_CSV" "$DST_CSV.bak.$(date +%Y%m%d-%H%M%S)"
  echo ">> CSV anterior movido para backup."
fi
cp "$SRC_CSV" "$DST_CSV"

LINHAS=$(($(wc -l <"$DST_CSV") - 1))
echo ">> Resultados copiados para $DST_CSV ($LINHAS linhas de dados; esperado 1200)."
echo ">> Proximo passo: python3 scripts/analyze.py"

