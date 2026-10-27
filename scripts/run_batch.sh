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
#   4. roda o binario compilado em 24 partes paralelas (estagio x protocolo x velocidade,
#      ver JOBS abaixo) e junta os resultados — isso sozinho já faz a
#      validação dos 3 protocolos e DOIS estágios da matriz experimental:
#        - base: 3 protocolos x 4 velocidades x NUM_RUNS repeticoes (lido do .cc), 20 nos
#        - extensao: a mesma matriz, 10 nos (variavel experimental do grupo)
#      (2 x 12 x NUM_RUNS execucoes no total).
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

# Execucao em paralelo: cada (estagio x protocolo x velocidade) vira um
# processo independente do mesmo binario (--stage / --protocol / --speed /
# --outDir), 24 partes no total (dividir tambem por velocidade equilibra a
# carga: um protocolo a 20 nos pode ser varias vezes mais lento que outro). O resultado de cada execucao nao depende da ordem nem de quais
# outras rodam no mesmo processo (streams isolados em MontarCenario), entao
# o CSV juntado e identico ao de uma rodada sequencial. JOBS = quantos
# processos ao mesmo tempo (padrao: nucleos - 2; JOBS=1 = sequencial; cada
# processo usa 1 nucleo, em prioridade baixa para nao pesar no resto do
# computador).
NUCLEOS=$(nproc 2>/dev/null || echo 4)
JOBS="${JOBS:-$(( NUCLEOS > 3 ? NUCLEOS - 2 : 1 ))}"
BIN=$(find "$NS3_DIR/build/scratch" -maxdepth 1 -type f -executable -name '*manet-routing*' | head -1)
if [[ -z "$BIN" ]]; then
  echo "ERRO: nao encontrei o binario compilado em $NS3_DIR/build/scratch/." >&2
  exit 1
fi
OUT="$NS3_DIR/resultado_simulacao"
PARTES="$NS3_DIR/resultado_simulacao_partes"
rm -rf "$OUT" "$PARTES"
mkdir -p "$OUT/flowmon" "$PARTES"

rodar_parte () {
  local stage="$1" proto="$2" speed="$3" dir="$PARTES/${1}_${2}_${3}"
  mkdir -p "$dir"
  if ! LD_LIBRARY_PATH="$NS3_DIR/build/lib" nice -n 19 "$BIN" --stage="$stage" \
       --protocol="$proto" --speed="$speed" --outDir="$dir/saida" >"$dir/stdout.txt" 2>&1; then
    echo "   FALHOU: $stage / $proto / $speed m/s (ver $dir/stdout.txt)" >&2
    return 1
  fi
  echo "   parte concluida: $stage / $proto / $speed m/s"
}
export -f rodar_parte
export NS3_DIR BIN PARTES

echo ">> Rodando a simulacao: 24 partes (20n/10n x aodv/olsr/dsdv x 1/5/10/20 m/s),"
echo ">> $JOBS em paralelo, $NUM_RUNS repeticoes por parte. O progresso detalhado de cada"
echo ">> parte fica em $PARTES/<estagio>_<protocolo>_<velocidade>/stdout.txt."
# ordem: as partes mais pesadas (20 nos, aodv) primeiro, para nao sobrarem no fim
for stage in 20n 10n; do
  for proto in aodv olsr dsdv; do
    for speed in 1 5 10 20; do echo "$stage $proto $speed"; done
  done
done | xargs -P "$JOBS" -L1 bash -c 'rodar_parte "$0" "$1" "$2"'

echo ">> Juntando as partes (mesma ordem de uma rodada sequencial: protocolo, velocidade)..."
for stage in 20n 10n; do
  csv="$OUT/resultados_${stage}.csv"
  head -n 1 "$PARTES/${stage}_aodv_1/saida/resultados_${stage}.csv" >"$csv"
  for proto in aodv olsr dsdv; do
    for speed in 1 5 10 20; do
      tail -n +2 "$PARTES/${stage}_${proto}_${speed}/saida/resultados_${stage}.csv" >>"$csv"
    done
  done
done
: >"$OUT/log.txt"
for stage in 20n 10n; do
  for proto in aodv olsr dsdv; do
    for speed in 1 5 10 20; do
      echo "################ parte: $stage / $proto / $speed m/s ################" >>"$OUT/log.txt"
      cat "$PARTES/${stage}_${proto}_${speed}/saida/log.txt" >>"$OUT/log.txt"
    done
  done
done
cp -r "$PARTES"/*/saida/flowmon/. "$OUT/flowmon/"

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
