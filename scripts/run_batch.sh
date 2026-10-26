#!/usr/bin/env bash
# =============================================================================
# run_batch.sh — automatiza a simulação completa
#
# O programa ns-3 (src/manet-routing.cc) já roda tudo sozinho quando
# executado (sem precisar de argumento nenhum). Este script só faz a ponte
# entre o ns-3 (onde o programa precisa estar, em $NS3_DIR/scratch/) e este
# projeto (onde os resultados devem ficar, em data/raw/):
#
#   1. move o conteúdo atual de data/raw/, data/processed/ e figures/ para
#      uma subpasta bkp/ dentro de cada uma (substitui o bkp/ da execução
#      anterior) — cada execução nova começa com essas 3 pastas limpas
#   2. liga src/manet-routing.cc a $NS3_DIR/scratch/ (symlink)
#   3. compila (./ns3 build)
#   4. roda (./ns3 run scratch/manet-routing) — isso sozinho já faz a
#      validação dos 3 protocolos e DOIS estágios da matriz experimental:
#        - base: 3 protocolos x 4 velocidades x 25 repeticoes, 20 nos
#        - extensao: a mesma matriz, 10 nos (variavel experimental do grupo)
#      600 execucoes no total.
#   5. copia de volta para data/raw/ deste projeto: resultados_20n.csv,
#      resultados_10n.csv, o log completo da execução (log.txt) e a amostra
#      de XML do FlowMonitor (flowmon/, 1 arquivo por combinação
#      protocolo x velocidade x estágio)
#   6. roda scripts/analyze.py sobre os CSVs copiados, gerando as tabelas
#      processadas (data/processed/) e os gráficos (figures/)
#
# Uso:
#   export NS3_DIR=$HOME/ns-3-allinone
#   bash scripts/run_batch.sh
#
# data/raw/bkp/, data/processed/bkp/ e figures/bkp/ guardam só a execução
# anterior (cada rodada substitui o bkp/ da rodada antes dela) — não é um
# histórico completo, é só para não perder o último resultado por engano.
#
# Com 25 repetições por combinação (em vez de 5) e dois estágios (20 nós +
# 10 nós) em vez de um, a simulação demora bem mais que uma matriz simples
# de 5 repetições. É normal o terminal ficar rodando por um tempo
# considerável antes de chegar ao fim da ETAPA 3/3.
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

# Move o conteudo atual de $dir (exceto a propria bkp/) para $dir/bkp/,
# substituindo o bkp/ de uma execucao anterior. Usado para as 3 pastas de
# saida (data/raw, data/processed, figures), que comecam limpas a cada
# execucao nova.
backup_old_outputs () {
  local dir="$1"
  mkdir -p "$dir"
  rm -rf "$dir/bkp"
  mkdir -p "$dir/bkp"
  find "$dir" -mindepth 1 -maxdepth 1 ! -name bkp -exec mv {} "$dir/bkp/" \;
}

echo ">> Movendo saida da execucao anterior para bkp/ (data/raw, data/processed, figures)..."
backup_old_outputs "$PROJECT_DIR/data/raw"
backup_old_outputs "$PROJECT_DIR/data/processed"
backup_old_outputs "$PROJECT_DIR/figures"

# Numero de repeticoes lido do proprio .cc (fonte unica da verdade), para o
# analyze.py validar e rotular os graficos com o mesmo valor.
NUM_RUNS=$(grep -oP 'NUM_RUNS\s*=\s*\K[0-9]+' "$PROJECT_DIR/src/manet-routing.cc" | head -1)
LINHAS_ESPERADAS=$((3 * 4 * NUM_RUNS * 4)) # protocolos x velocidades x repeticoes x 4 fluxos

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

echo ">> Compilando (./ns3 build manet-routing)..."
# So builda o alvo manet-routing (nao a arvore toda): outros alvos soltos em
# scratch/ (ex.: projetos externos do usuario) nao devem poder quebrar este build.
(cd "$NS3_DIR" && ./ns3 build manet-routing)

echo ">> Rodando a simulacao (validacao + ETAPA 2/3 base 20 nos + ETAPA 3/3 extensao 10 nos)."
echo ">> Isso demora bastante ($NUM_RUNS repeticoes x 2 estagios); o terminal mostra uma linha"
echo ">> de progresso por protocolo x velocidade (nao uma por execucao), para cada estagio."
(cd "$NS3_DIR" && ./ns3 run scratch/manet-routing)

SRC_LOG="$NS3_DIR/resultado_simulacao/log.txt"
SRC_FLOWMON="$NS3_DIR/resultado_simulacao/flowmon"
DST_LOG="$PROJECT_DIR/data/raw/log.txt"
DST_FLOWMON="$PROJECT_DIR/data/raw/flowmon"

mkdir -p "$PROJECT_DIR/data/raw"

# Copia um CSV de resultado_simulacao/ para data/raw/. data/raw/ ja comeca
# limpo (ver backup_old_outputs, acima), entao nao ha arquivo anterior pra
# preservar aqui.
copy_csv () {
  local nome="$1" # ex.: resultados_20n.csv
  local src="$NS3_DIR/resultado_simulacao/$nome"
  local dst="$PROJECT_DIR/data/raw/$nome"

  if [[ ! -f "$src" ]]; then
    echo "ERRO: simulacao terminou mas nao encontrei $src" >&2
    exit 1
  fi
  cp "$src" "$dst"
}

copy_csv "resultados_20n.csv"
copy_csv "resultados_10n.csv"

if [[ -f "$SRC_LOG" ]]; then
  cp "$SRC_LOG" "$DST_LOG"
else
  echo "AVISO: nao encontrei $SRC_LOG (log.txt nao foi copiado)." >&2
fi

if [[ -d "$SRC_FLOWMON" ]]; then
  cp -r "$SRC_FLOWMON" "$DST_FLOWMON"
else
  echo "AVISO: nao encontrei $SRC_FLOWMON (XML do FlowMonitor nao foi copiado)." >&2
fi

LINHAS_20N=$(($(wc -l <"$PROJECT_DIR/data/raw/resultados_20n.csv") - 1))
LINHAS_10N=$(($(wc -l <"$PROJECT_DIR/data/raw/resultados_10n.csv") - 1))
echo ">> Resultados copiados para $PROJECT_DIR/data/raw/:"
echo "   - resultados_20n.csv ($LINHAS_20N linhas de dados; esperado $LINHAS_ESPERADAS) — matriz-base"
echo "   - resultados_10n.csv ($LINHAS_10N linhas de dados; esperado $LINHAS_ESPERADAS) — extensao"
echo "   - log.txt (log completo da execucao, os dois estagios)"
echo "   - flowmon/ (amostra de XML do FlowMonitor, 1 por combinacao protocolo x velocidade x estagio)"

echo ">> Gerando graficos e tabelas (python3 scripts/analyze.py --expected-runs "$NUM_RUNS")..."
(cd "$PROJECT_DIR" && python3 scripts/analyze.py --expected-runs "$NUM_RUNS")
