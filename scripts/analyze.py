
"""
analyze.py - análise dos CSVs gerados pelo ns-3 (scratch/manet-routing.cc)

Processa dois conjuntos de dados, um por estágio da matriz experimental:
  - base (tag "20n"):      data/raw/resultados_20n.csv (20 nós)
  - extensão (tag "10n"):  data/raw/resultados_10n.csv (10 nós, variável
                            experimental própria do grupo)

Cada CSV tem 1 linha por fluxo (protocol,speed,run,flowId,txPackets,
rxPackets,lostPackets,rxBytes,throughputMbps,pdrPct,delayMs,jitterMs). Para
cada um: valida, agrega os 4 fluxos por execução, calcula média/desvio/IC 95%
sobre as repetições, e gera os 4 gráficos obrigatórios (PDR, throughput,
delay, jitter x velocidade) + boxplots de dispersão + tabelas processadas,
com o tag do estágio no nome do arquivo. Se os dois estágios tiverem rodado
com sucesso, gera também 4 gráficos comparando 20 vs 10 nós.

A extensão (10n) é opcional: se data/raw/resultados_10n.csv ainda não
existir, o script processa só a base e avisa que a comparação foi pulada —
não é um erro (permite analisar a base antes de rodar a extensão).

Saídas (por estágio, <tag> = 20n ou 10n):
    data/processed/per_run_<tag>.csv          1 linha por execução (já agregada)
    data/processed/summary_<tag>.csv          1 linha por (protocolo, velocidade)
    data/processed/validation_report_<tag>.txt  relatório da validação
    figures/<metrica>_vs_speed_<tag>.png/.pdf 4 gráficos obrigatórios
    figures/boxplot_<metrica>_<tag>.png       dispersão das 60 repetições

    data/processed/descriptive_<tag>.csv      média, mediana, desvio, IQR, CV, assimetria,
                                              nº de pontos além de 1,5xIQR (só informativo:
                                              nada é removido dos dados)
    data/processed/stats_tests_<tag>.csv      Shapiro-Wilk, Levene, Kruskal-Wallis entre
                                              protocolos e entre velocidades, e comparações
                                              par a par (Mann-Whitney, correção de Holm)

Saídas de comparação (só se os dois estágios tiverem dados válidos):
    figures/<metrica>_nodes_comparison.png/.pdf  20 vs 10 nós, por protocolo
    data/processed/stats_nodes_comparison.csv    Mann-Whitney 20 vs 10 nós, por protocolo x velocidade

Uso:
    python3 scripts/analyze.py                      # defaults: resultados_20n.csv / resultados_10n.csv
    python3 scripts/analyze.py --raw outro/caminho.csv --raw-ext outro/extensao.csv
    python3 scripts/analyze.py --errorbars std       # barras de erro = desvio padrão em vez de IC 95%
    python3 scripts/analyze.py --expected-runs 5     # análise de uma rodada de teste com 5 repetições
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import cast

import itertools

import matplotlib
import numpy as np
import pandas as pd
from scipy import stats

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# ------------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parent.parent  # raiz do projeto (manet-ns3/)
PROTOCOLS = ["AODV", "OLSR", "DSDV"]
SPEEDS = [1.0, 5.0, 10.0, 20.0]
EXPECTED_RUNS = 60  # sobrescrito por --expected-runs (precisa bater com NUM_RUNS do .cc)
ALPHA = 0.05
FLOWS_PER_RUN = 4
COLORS = {"AODV": "#1f77b4", "OLSR": "#d62728", "DSDV": "#2ca02c"}
MARKERS = {"AODV": "o", "OLSR": "s", "DSDV": "^"}
CONFIDENCE = 0.95

RAW_COLS = ["protocol", "speed", "run", "flowId", "txPackets", "rxPackets", "lostPackets",
            "rxBytes", "throughputMbps", "pdrPct", "delayMs", "jitterMs"]

# t de Student bicaudal 95% (fallback se scipy não estiver instalado); com 5 repetições, g.l.=4 -> 2,776
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262}

METRICS = {
    # coluna agregada -> (rótulo eixo Y, título, sufixo do arquivo)
    "pdrPct": ("PDR (%)", "Taxa de entrega de pacotes (PDR)", "pdr"),
    "throughputMbps": ("Throughput (Mbps)", "Throughput", "throughput"),
    "delayMs": ("Atraso médio (ms)", "Atraso médio fim-a-fim", "delay"),
    "jitterMs": ("Jitter médio (ms)", "Jitter médio", "jitter"),
}

plt.rcParams.update({
    "font.size": 12, "axes.titlesize": 13, "axes.labelsize": 12, "legend.fontsize": 11,
    "axes.grid": True, "grid.alpha": 0.3, "savefig.dpi": 300, "figure.autolayout": True,
})


# ------------------------------------------------------------------------------
# Pequeno utilitário de tipagem.
#
# pandas-stubs só consegue provar que df[nome] devolve uma Series (e não um
# DataFrame) quando "nome" é uma string literal fixa, tipo df["protocol"].
# Quando o nome da coluna vem de uma variável (como o "c" nos laços abaixo),
# o checador de tipos não tem como garantir isso e passa a tratar o retorno
# como ambíguo — é por isso que, por exemplo, "df[c].isna().any()" aparece
# sublinhado no editor, mesmo o código estando correto em tempo de execução.
# A função col() só declara explicitamente, num único lugar, que o retorno é
# sempre uma Series (o que é verdade: todo CSV lido aqui tem nomes de coluna
# únicos). cast() não faz nenhuma conversão real — é só uma anotação para o
# verificador de tipos, descartada quando o programa roda de verdade.
# ------------------------------------------------------------------------------
def col(df: pd.DataFrame, name: str) -> pd.Series:
    return cast(pd.Series, df[name])


# ------------------------------------------------------------------------------
# Estatística
# ------------------------------------------------------------------------------
def t_critical(dof: int) -> float:
    """Valor crítico t de Student bicaudal, para o nível de confiança CONFIDENCE."""
    try:
        return float(stats.t.ppf(0.5 + CONFIDENCE / 2.0, dof))
    except Exception:
        return T95.get(dof, 1.960)


def describe(values: pd.Series) -> dict:
    """Média, desvio padrão amostral e meia-largura do IC 95% de uma série de valores."""
    v = values.dropna().astype(float)
    n = int(v.size)
    if n == 0:
        return {"n": 0, "mean": np.nan, "std": np.nan, "ci_half": np.nan}
    mean = float(v.mean())
    if n == 1:
        return {"n": 1, "mean": mean, "std": np.nan, "ci_half": np.nan}
    std = float(v.std(ddof=1))
    half = t_critical(n - 1) * std / np.sqrt(n)
    return {"n": n, "mean": mean, "std": std, "ci_half": half}


# ------------------------------------------------------------------------------
# Validação do CSV bruto
# ------------------------------------------------------------------------------
class Report:
    """Acumula mensagens de informação, aviso e erro, e monta o relatório final."""

    def __init__(self):
        self.errors, self.warnings, self.info = [], [], []

    def error(self, m): self.errors.append(m)
    def warn(self, m): self.warnings.append(m)
    def note(self, m): self.info.append(m)

    def text(self) -> str:
        out = ["=" * 78, "RELATÓRIO DE VALIDAÇÃO", "=" * 78, "",
               "INFORMAÇÕES:"] + [f"  - {m}" for m in self.info] + [
               "", f"AVISOS ({len(self.warnings)}):"] + [f"  ! {m}" for m in self.warnings] + [
               "", f"ERROS ({len(self.errors)}):"] + [f"  X {m}" for m in self.errors] + [
               "", "RESULTADO: " + ("REPROVADO (há erros)" if self.errors else "APROVADO")]
        return "\n".join(out)


def load_and_validate(path: Path, rep: Report) -> pd.DataFrame | None:
    """
    Lê o CSV e roda todas as checagens de sanidade antes de confiar nos dados:
    colunas presentes, tipos numéricos, protocolos e velocidades esperados,
    matriz experimental completa (3 protocolos x 4 velocidades x EXPECTED_RUNS
    runs, cada execução com exatamente 4 fluxos), e valores fisicamente impossíveis
    (rx maior que tx, PDR fora de 0-100, etc.). Retorna None se o arquivo não
    puder ser lido; nesse caso rep.errors já descreve o motivo.
    """
    if not path.exists():
        rep.error(f"arquivo não encontrado: {path}")
        return None
    df = pd.read_csv(path)
    missing = [c for c in RAW_COLS if c not in df.columns]
    if missing:
        rep.error(f"colunas ausentes no CSV: {missing}")
        return None
    if df.empty:
        rep.error("CSV vazio.")
        return None

    df["protocol"] = col(df, "protocol").astype(str).str.strip().str.upper()
    for c in ["speed", "run", "flowId", "txPackets", "rxPackets", "lostPackets", "rxBytes",
              "throughputMbps", "pdrPct", "delayMs", "jitterMs"]:
        df[c] = pd.to_numeric(col(df, c), errors="coerce")
        if col(df, c).isna().any():
            rep.error(f"coluna '{c}' tem valores não numéricos/ausentes.")

    rep.note(f"{len(df)} linhas lidas de {path}")

    bad_proto = sorted(set(col(df, "protocol")) - set(PROTOCOLS))
    if bad_proto:
        rep.error(f"protocolos inesperados: {bad_proto} (esperado {PROTOCOLS})")

    df["speed"] = col(df, "speed").round(6)
    bad_speed = sorted(set(col(df, "speed")) - set(SPEEDS))
    if bad_speed:
        rep.error(f"velocidades fora de {SPEEDS}: {bad_speed}")

    # Cada execução (protocolo + velocidade + run) precisa ter exatamente
    # FLOWS_PER_RUN linhas — uma por fluxo UDP gravado pelo .cc.
    contagem = cast(pd.Series, df.groupby(["protocol", "speed", "run"]).size())
    errada = contagem[contagem != FLOWS_PER_RUN]
    if len(errada):
        rep.error(f"{len(errada)} execuções sem exatamente {FLOWS_PER_RUN} linhas (fluxos): "
                  f"{dict(list(errada.items())[:5])}")

    # A matriz experimental completa tem que ter as 60 combinações de
    # protocolo x velocidade x run — nem a mais, nem a menos.
    exec_presentes = set(map(tuple, df[["protocol", "speed", "run"]].drop_duplicates().values))
    exec_esperadas = {(p, s, r) for p in PROTOCOLS for s in SPEEDS for r in range(1, EXPECTED_RUNS + 1)}
    faltando = sorted(exec_esperadas - exec_presentes)
    sobrando = sorted(exec_presentes - exec_esperadas)
    if faltando:
        rep.error(f"{len(faltando)} execuções ausentes (ex.: {faltando[:5]})")
    if sobrando:
        rep.error(f"{len(sobrando)} execuções fora do esperado (ex.: {sobrando[:5]})")
    rep.note(f"execuções: {len(exec_presentes)} (esperado {len(exec_esperadas)})")

    reps = cast(pd.Series, df.groupby(["protocol", "speed"])["run"].nunique())
    incons = reps[reps != EXPECTED_RUNS]
    if len(incons):
        rep.error(f"número de repetições inconsistente (esperado {EXPECTED_RUNS}): "
                  f"{incons.to_dict()}")

    # Valores que não podem acontecer fisicamente, independente de qual
    # protocolo ou cenário gerou os dados.
    for c in ["txPackets", "rxPackets", "lostPackets", "rxBytes", "throughputMbps", "delayMs",
              "jitterMs"]:
        if (col(df, c) < 0).any():
            rep.error(f"valores negativos em '{c}'.")
    if (col(df, "rxPackets") > col(df, "txPackets")).any():
        rep.error("rxPackets > txPackets em alguma linha.")
    if ((col(df, "pdrPct") < 0) | (col(df, "pdrPct") > 100.0001)).any():
        rep.error("pdrPct fora de [0,100] em alguma linha.")
    if ((col(df, "rxPackets") == 0) & ((col(df, "delayMs") != 0) | (col(df, "jitterMs") != 0) |
                                       (col(df, "throughputMbps") != 0))).any():
        rep.error("fluxo com rxPackets = 0 mas delay/jitter/throughput diferente de 0 "
                  "(o .cc grava 0 quando a métrica é indefinida; confira a geração do CSV).")
    if (col(df, "txPackets") == 0).any():
        rep.warn(f"{(col(df, 'txPackets') == 0).sum()} linhas com txPackets = 0 "
                 "(nenhum pacote saiu nesse fluxo).")

    sem_rx = df[col(df, "rxPackets") == 0]
    if len(sem_rx):
        rep.warn(f"{len(sem_rx)}/{len(df)} fluxos sem nenhuma recepção, por protocolo: "
                 f"{sem_rx.groupby('protocol').size().to_dict()}")
    zero_exec = cast(pd.Series, df.groupby(["protocol", "speed", "run"])["rxPackets"].sum())
    zero_exec = zero_exec[zero_exec == 0]
    if len(zero_exec):
        rep.warn(f"{len(zero_exec)} execuções com ZERO pacotes recebidos (todos os 4 fluxos).")

    # No cenário usado aqui, todo fluxo deveria gerar o mesmo número de
    # pacotes (mesmo warmup/guard/intervalo em todas as execuções); se
    # txPackets varia, vale checar se o cenário realmente ficou constante.
    tx_unicos = col(df, "txPackets").unique()
    if len(tx_unicos) > 1:
        rep.warn(f"txPackets varia entre execuções: {sorted(tx_unicos)} "
                 "(deveria ser sempre o mesmo valor, calculado a partir de "
                 "warmup/guard/intervalo do pacote).")

    return df


# ------------------------------------------------------------------------------
# Agregação: 4 fluxos -> 1 valor por execução
#
# Regra adotada (a mesma para todas as combinações protocolo x velocidade):
#   tx, rx, lost, rxBytes         : soma dos 4 fluxos
#   PDR (%)                       : 100 * soma(rx) / soma(tx)
#   Throughput (Mbps)             : soma dos throughputMbps dos 4 fluxos
#                                    (fluxos concorrem na mesma janela de tempo,
#                                    então a soma das vazões individuais é a
#                                    vazão agregada da execução)
#   Delay médio (ms)              : média de delayMs PONDERADA por rxPackets
#                                    de cada fluxo (fluxo que recebeu mais
#                                    pacotes pesa mais na média); indefinido se nenhum
#                                    fluxo recebeu pacote
#   Jitter médio (ms)             : média de jitterMs ponderada por
#                                    max(rxPackets-1, 0) de cada fluxo
#
# Delay e jitter de uma execução sem NENHUM pacote recebido (ou, no jitter,
# sem pelo menos 2 pacotes num mesmo fluxo) são INDEFINIDOS, não zero: o .cc
# grava 0 só como placeholder. Aqui viram NaN e ficam de fora da média dessa
# métrica (PDR e throughput dessa execução continuam valendo 0, que aí é um
# valor real).
# ------------------------------------------------------------------------------
def aggregate_runs(df: pd.DataFrame) -> pd.DataFrame:
    rows = []
    for (proto, speed, run), g in df.groupby(["protocol", "speed", "run"], sort=True):
        tx = float(g["txPackets"].sum())
        rx = float(g["rxPackets"].sum())
        lost = float(g["lostPackets"].sum())
        rx_bytes = float(g["rxBytes"].sum())

        pdr = 100.0 * rx / tx if tx > 0 else 0.0
        throughput = float(g["throughputMbps"].sum())

        peso_delay = g["rxPackets"]
        delay = float((g["delayMs"] * peso_delay).sum() / peso_delay.sum()) if peso_delay.sum() > 0 else np.nan

        peso_jitter = (g["rxPackets"] - 1).clip(lower=0)
        jitter = float((g["jitterMs"] * peso_jitter).sum() / peso_jitter.sum()) if peso_jitter.sum() > 0 else np.nan

        rows.append({"protocol": proto, "speed": float(speed), "run": int(run),
                     "txPackets": tx, "rxPackets": rx, "lostPackets": lost, "rxBytes": rx_bytes,
                     "pdrPct": pdr, "throughputMbps": throughput, "delayMs": delay, "jitterMs": jitter})
    out = pd.DataFrame(rows)
    out["protocol"] = pd.Categorical(out["protocol"], categories=PROTOCOLS, ordered=True)
    return out.sort_values(["protocol", "speed", "run"]).reset_index(drop=True)


def summarize(per_run: pd.DataFrame) -> pd.DataFrame:
    """Para cada (protocolo, velocidade): n de repetições, média, desvio padrão e IC 95% de cada métrica."""
    rows = []
    for (proto, speed), g in per_run.groupby(["protocol", "speed"], sort=True, observed=True):
        row = {"protocol": str(proto), "speed": float(speed), "n_runs": int(g["run"].nunique()),
               "confidence": CONFIDENCE}
        for m in METRICS:
            d = describe(g[m])
            for k, v in d.items():
                row[f"{m}_{k}"] = v
        rows.append(row)
    out = pd.DataFrame(rows)
    out["protocol"] = pd.Categorical(out["protocol"], categories=PROTOCOLS, ordered=True)
    return out.sort_values(["protocol", "speed"]).reset_index(drop=True)


# ------------------------------------------------------------------------------
# Estatística descritiva e testes de hipótese
#
# Nada aqui remove dados: os pontos além de 1,5xIQR são só CONTADOS (para
# saber se a média está sendo puxada por poucos valores extremos), e todas as
# execuções continuam entrando em médias, ICs e testes.
#
# Por que Kruskal-Wallis / Mann-Whitney (e não ANOVA / t): delay e jitter são
# assimétricos (cauda longa), e o Shapiro-Wilk de cada célula está no CSV de
# testes para conferir. Os testes por postos não assumem normalidade e não
# são afetados por poucos valores extremos. Levene (mediana) checa a
# homogeneidade de variâncias entre protocolos.
# ------------------------------------------------------------------------------
def holm(pvals: list[float]) -> list[float]:
    """Correção de Holm (controla o erro tipo I familiar nas comparações par a par)."""
    m = len(pvals)
    ordem = np.argsort(pvals)
    ajustado = np.empty(m)
    maximo = 0.0
    for rank, i in enumerate(ordem):
        maximo = max(maximo, (m - rank) * pvals[i])
        ajustado[i] = min(1.0, maximo)
    return ajustado.tolist()


def cliffs_delta(a: np.ndarray, b: np.ndarray) -> float:
    """Tamanho de efeito de Cliff: P(a>b) - P(a<b), em [-1, 1]."""
    u = stats.mannwhitneyu(a, b, alternative="two-sided").statistic
    return float(2.0 * u / (a.size * b.size) - 1.0)


def significance_stars(p: float) -> str:
    return "***" if p < 0.001 else "**" if p < 0.01 else "*" if p < ALPHA else ""


def _values(per_run: pd.DataFrame, metric: str, **filtros) -> np.ndarray:
    d = per_run
    for k, v in filtros.items():
        d = d[d[k] == v]
    return d[metric].dropna().to_numpy(dtype=float)


def descriptive(per_run: pd.DataFrame) -> pd.DataFrame:
    """Uma linha por (protocolo, velocidade, métrica) com estatística descritiva completa."""
    rows = []
    for proto in PROTOCOLS:
        for speed in SPEEDS:
            for m in METRICS:
                v = _values(per_run, m, protocol=proto, speed=speed)
                if v.size == 0:
                    continue
                q1, med, q3 = np.percentile(v, [25, 50, 75])
                iqr = q3 - q1
                fora = int(((v < q1 - 1.5 * iqr) | (v > q3 + 1.5 * iqr)).sum())
                std = float(v.std(ddof=1)) if v.size > 1 else np.nan
                rows.append({"protocol": proto, "speed": speed, "metric": m, "n": int(v.size),
                             "mean": float(v.mean()), "median": float(med), "std": std,
                             "cv_pct": 100.0 * std / v.mean() if v.mean() else np.nan,
                             "q1": float(q1), "q3": float(q3), "iqr": float(iqr),
                             "min": float(v.min()), "max": float(v.max()),
                             "skewness": float(stats.skew(v, bias=False)) if v.size > 2 else np.nan,
                             "n_beyond_1.5iqr": fora})
    return pd.DataFrame(rows)


def run_tests(per_run: pd.DataFrame) -> pd.DataFrame:
    """
    Formato longo: metric, test, group, statistic, p_value, p_adjusted, effect_size, significant.
      shapiro_wilk   : normalidade de cada (protocolo, velocidade)
      levene         : homogeneidade de variâncias entre protocolos, por velocidade
      kruskal_wallis : diferença entre protocolos, por velocidade (efeito = epsilon²)
                       e diferença entre velocidades, por protocolo
      mann_whitney   : pares de protocolos por velocidade, p ajustado por Holm
                       (efeito = delta de Cliff)
    """
    rows = []

    def add(metric, test, group, stat, p, p_adj=np.nan, effect=np.nan):
        p_dec = p if np.isnan(p_adj) else p_adj
        rows.append({"metric": metric, "test": test, "group": group, "statistic": stat,
                     "p_value": p, "p_adjusted": p_adj, "effect_size": effect,
                     "significant": bool(p_dec < ALPHA)})

    for m in METRICS:
        for proto in PROTOCOLS:
            for speed in SPEEDS:
                v = _values(per_run, m, protocol=proto, speed=speed)
                if v.size >= 3 and np.ptp(v) > 0:
                    r = stats.shapiro(v)
                    add(m, "shapiro_wilk", f"{proto} @ {speed:g} m/s", float(r.statistic), float(r.pvalue))
        for speed in SPEEDS:
            grupos = {pr: _values(per_run, m, protocol=pr, speed=speed) for pr in PROTOCOLS}
            validos = [g for g in grupos.values() if g.size >= 2]
            if len(validos) >= 2:
                r = stats.levene(*validos, center="median")
                add(m, "levene", f"protocolos @ {speed:g} m/s", float(r.statistic), float(r.pvalue))
                h = stats.kruskal(*validos)
                n_tot = sum(g.size for g in validos)
                add(m, "kruskal_wallis", f"protocolos @ {speed:g} m/s", float(h.statistic),
                    float(h.pvalue), effect=float(h.statistic / (n_tot - 1)))
            pares = [(a, b) for a, b in itertools.combinations(PROTOCOLS, 2)
                     if grupos[a].size and grupos[b].size]
            ps = [float(stats.mannwhitneyu(grupos[a], grupos[b], alternative="two-sided").pvalue)
                  for a, b in pares]
            for (a, b), pv, pa in zip(pares, ps, holm(ps) if ps else []):
                u = stats.mannwhitneyu(grupos[a], grupos[b], alternative="two-sided")
                add(m, "mann_whitney", f"{a} vs {b} @ {speed:g} m/s", float(u.statistic), pv, pa,
                    cliffs_delta(grupos[a], grupos[b]))
        for proto in PROTOCOLS:
            grupos = [_values(per_run, m, protocol=proto, speed=sp) for sp in SPEEDS]
            grupos = [g for g in grupos if g.size >= 2]
            if len(grupos) >= 2:
                h = stats.kruskal(*grupos)
                n_tot = sum(g.size for g in grupos)
                add(m, "kruskal_wallis", f"velocidades ({proto})", float(h.statistic),
                    float(h.pvalue), effect=float(h.statistic / (n_tot - 1)))
    return pd.DataFrame(rows)


def run_nodes_tests(per_run_20n: pd.DataFrame, per_run_10n: pd.DataFrame) -> pd.DataFrame:
    """Mann-Whitney 20 vs 10 nós, por (métrica, protocolo, velocidade); Holm dentro de cada métrica."""
    rows = []
    for m in METRICS:
        bloco = []
        for proto in PROTOCOLS:
            for speed in SPEEDS:
                a = _values(per_run_20n, m, protocol=proto, speed=speed)
                b = _values(per_run_10n, m, protocol=proto, speed=speed)
                if a.size and b.size:
                    u = stats.mannwhitneyu(a, b, alternative="two-sided")
                    bloco.append({"metric": m, "protocol": proto, "speed": speed,
                                  "statistic": float(u.statistic), "p_value": float(u.pvalue),
                                  "cliffs_delta_20n_vs_10n": cliffs_delta(a, b)})
        for r, pa in zip(bloco, holm([r["p_value"] for r in bloco]) if bloco else []):
            r["p_adjusted"] = pa
            r["significant"] = bool(pa < ALPHA)
        rows += bloco
    return pd.DataFrame(rows)


def kruskal_stars(tests: pd.DataFrame, metric: str) -> dict[float, str]:
    """Estrelas de significância (Kruskal-Wallis entre protocolos) por velocidade, para anotar os gráficos."""
    out = {}
    sel = tests[(tests["metric"] == metric) & (tests["test"] == "kruskal_wallis")]
    for speed in SPEEDS:
        r = sel[sel["group"] == f"protocolos @ {speed:g} m/s"]
        if len(r):
            out[speed] = significance_stars(float(r["p_value"].iloc[0]))
    return out


def annotate_stars(ax, stars: dict[float, str]) -> None:
    """Escreve as estrelas no topo do gráfico, acima de cada velocidade."""
    for speed, st in stars.items():
        if st:
            ax.annotate(st, xy=(speed, 1.0), xycoords=("data", "axes fraction"), ha="center",
                        va="bottom", fontsize=14, fontweight="bold", color="black")


# ------------------------------------------------------------------------------
# Gráficos
# ------------------------------------------------------------------------------
def plot_metric(summary: pd.DataFrame, metric: str, errorbars: str, figures_dir: Path,
                 tag: str, tests: pd.DataFrame) -> list[Path]:
    """
    Gráfico de linha de uma métrica em função da velocidade, com uma série
    por protocolo e barras de erro (IC 95% por padrão, ou desvio padrão se
    errorbars="std"). Salva em .png (visualização rápida) e .pdf (vetorial,
    melhor para incluir num documento). `tag` (ex. "20n"/"10n") identifica o
    estágio no nome do arquivo.
    """
    ylabel, title, suffix = METRICS[metric]
    fig, ax = plt.subplots(figsize=(7.2, 5))

    for proto in PROTOCOLS:
        sub = summary[summary["protocol"] == proto].sort_values("speed")
        if sub.empty:
            continue
        xs = sub["speed"].to_numpy(dtype=float)
        ys = sub[f"{metric}_mean"].to_numpy(dtype=float)
        err_col = f"{metric}_ci_half" if errorbars == "ci" else f"{metric}_std"
        yerr = np.nan_to_num(sub[err_col].to_numpy(dtype=float), nan=0.0)
        ax.errorbar(xs, ys, yerr=yerr, label=proto, color=COLORS[proto], marker=MARKERS[proto],
                    markersize=7, linewidth=2, capsize=4, elinewidth=1.3)

    ax.set_xlabel("Velocidade dos nós (m/s)")
    ax.set_ylabel(ylabel)
    ax.set_title(title, pad=22)
    ax.set_xticks(SPEEDS)
    if metric == "pdrPct":
        ax.set_ylim(0, 100)  # PDR é um percentual: eixo sempre fixo em [0,100]
    else:
        ax.set_ylim(bottom=0)
    ax.legend(title="Protocolo")
    annotate_stars(ax, kruskal_stars(tests, metric))

    paths = []
    for ext in ("png", "pdf"):
        p = figures_dir / f"{suffix}_vs_speed_{tag}.{ext}"
        fig.savefig(p, bbox_inches="tight")
        paths.append(p)
    plt.close(fig)
    return paths


def boxplot_metric(per_run: pd.DataFrame, metric: str, figures_dir: Path, tag: str,
                   tests: pd.DataFrame) -> Path:
    """
    Boxplot de uma métrica por velocidade, com os 3 protocolos lado a lado em
    cada posição. Diferente do gráfico de linha (que só mostra média ± erro),
    aqui dá para ver a distribuição completa das repetições — útil para
    perceber assimetria ou outliers que a média sozinha esconde. `tag`
    (ex. "20n"/"10n") identifica o estágio no nome do arquivo.
    """
    ylabel, title, suffix = METRICS[metric]
    fig, ax = plt.subplots(figsize=(9, 5.5))
    largura = 0.25
    pos_base = np.arange(len(SPEEDS))

    for i, proto in enumerate(PROTOCOLS):
        sub = per_run[per_run["protocol"] == proto]
        valores = [sub.loc[sub["speed"] == s, metric].dropna().values for s in SPEEDS]
        pos = pos_base + (i - 1) * largura
        bp = ax.boxplot(valores, positions=pos, widths=largura * 0.9, patch_artist=True,
                        manage_ticks=False)
        for caixa in bp["boxes"]:
            caixa.set_facecolor(COLORS[proto])
            caixa.set_alpha(0.6)
        for mediana in bp["medians"]:
            mediana.set_color("black")

    ax.set_xticks(pos_base)
    ax.set_xticklabels([f"{s:g}" for s in SPEEDS])
    ax.set_xlabel("Velocidade dos nós (m/s)")
    ax.set_ylabel(ylabel)
    ax.set_title(f"{title} — dispersão entre as {EXPECTED_RUNS} repetições", pad=22)
    # boxplot não gera legenda automática por cor; montamos uma manualmente
    handles = [plt.Rectangle((0, 0), 1, 1, facecolor=COLORS[p], alpha=0.6, label=p) for p in PROTOCOLS]
    ax.legend(handles=handles, title="Protocolo")
    for speed, st in kruskal_stars(tests, metric).items():
        if st:
            ax.annotate(st, xy=(SPEEDS.index(speed), 1.0), xycoords=("data", "axes fraction"),
                        ha="center", va="bottom", fontsize=14, fontweight="bold")

    p = figures_dir / f"boxplot_{suffix}_{tag}.png"
    fig.savefig(p, bbox_inches="tight")
    plt.close(fig)
    return p


def plot_nodes_comparison(summary_20n: pd.DataFrame, summary_10n: pd.DataFrame, metric: str,
                           errorbars: str, figures_dir: Path) -> list[Path]:
    """
    Gráfico de linha comparando os dois estágios (20 vs 10 nós): uma série
    por combinação protocolo x nº de nós (mesma cor por protocolo, traço
    sólido = 20 nós, tracejado = 10 nós), com barras de erro.
    """
    ylabel, title, suffix = METRICS[metric]
    fig, ax = plt.subplots(figsize=(7.5, 5.2))
    err_col = f"{metric}_ci_half" if errorbars == "ci" else f"{metric}_std"

    # handles de legenda montados à parte (em vez de deixar o legend() puxar
    # os da ErrorbarContainer): os caps do errorbar ocupam quase todo o
    # "handle" da legenda, deixando pouquíssimo espaço pro traço em si — daí
    # sólido (20 nós) e tracejado (10 nós) ficarem visualmente idênticos ali,
    # mesmo as linhas do gráfico estando corretas.
    handles = []
    for nodes_label, summary, linestyle in (("20 nós", summary_20n, "-"), ("10 nós", summary_10n, "--")):
        for proto in PROTOCOLS:
            sub = summary[summary["protocol"] == proto].sort_values("speed")
            if sub.empty:
                continue
            xs = sub["speed"].to_numpy(dtype=float)
            ys = sub[f"{metric}_mean"].to_numpy(dtype=float)
            yerr = np.nan_to_num(sub[err_col].to_numpy(dtype=float), nan=0.0)
            ax.errorbar(xs, ys, yerr=yerr, color=COLORS[proto],
                        marker=MARKERS[proto], linestyle=linestyle, markersize=6, linewidth=1.8,
                        capsize=3, elinewidth=1.1)
            handles.append(Line2D([], [], color=COLORS[proto], marker=MARKERS[proto],
                                   linestyle=linestyle, markersize=6, linewidth=1.8,
                                   label=f"{proto} ({nodes_label})"))

    ax.set_xlabel("Velocidade dos nós (m/s)")
    ax.set_ylabel(ylabel)
    ax.set_title(f"{title} — efeito do número de nós (20 vs 10)", pad=22)
    ax.set_xticks(SPEEDS)
    if metric == "pdrPct":
        ax.set_ylim(0, 100)
    else:
        ax.set_ylim(bottom=0)
    ax.legend(handles=handles, title="Protocolo (nós)", fontsize=9, ncol=2, handlelength=3.2)

    paths = []
    for ext in ("png", "pdf"):
        p = figures_dir / f"{suffix}_nodes_comparison.{ext}"
        fig.savefig(p, bbox_inches="tight")
        paths.append(p)
    plt.close(fig)
    return paths


# ------------------------------------------------------------------------------
def run_pipeline(raw_path: Path, tag: str, label: str, processed_dir: Path, figures_dir: Path,
                  errorbars: str, force: bool) -> tuple[pd.DataFrame, pd.DataFrame] | None:
    """
    Roda o pipeline completo (validar -> agregar -> resumir -> tabelas ->
    4 gráficos -> boxplots) para um único estágio (tag "20n" ou "10n").
    Devolve (per_run, resumo) em caso de sucesso, ou None se a validação
    reprovou (ou o arquivo não existe/está vazio).
    """
    print(f"\n{'=' * 78}\nANALISANDO: {label} ({raw_path})\n{'=' * 78}")

    rep = Report()
    df = load_and_validate(raw_path, rep)

    report_path = processed_dir / f"validation_report_{tag}.txt"
    report_path.write_text(rep.text() + "\n", encoding="utf-8")
    print(rep.text())
    print(f"\n(relatório salvo em {report_path})")

    if df is None or (rep.errors and not force):
        if rep.errors:
            print(f"\n{label}: análise interrompida (corrija os erros acima, ou use --force "
                  "só para depurar).")
        return None

    per_run = aggregate_runs(df)
    summary = summarize(per_run)

    per_run_path = processed_dir / f"per_run_{tag}.csv"
    summary_path = processed_dir / f"summary_{tag}.csv"
    per_run.to_csv(per_run_path, index=False, float_format="%.6g")
    summary.to_csv(summary_path, index=False, float_format="%.6g")
    print(f"\nTabela por execução : {per_run_path}")
    print(f"Tabela de resumo    : {summary_path}")

    print("\nResumo (média ± IC95%, n repetições):")
    linhas = summary[["protocol", "speed", "n_runs"]].copy()
    for m in METRICS:
        linhas[m] = [
            f"{r[f'{m}_mean']:.4g} ± {r[f'{m}_ci_half']:.3g}" if not np.isnan(r[f"{m}_mean"]) else "NA"
            for _, r in summary.iterrows()
        ]
    with pd.option_context("display.width", 200, "display.max_columns", 20):
        print(linhas.to_string(index=False))

    desc_path = processed_dir / f"descriptive_{tag}.csv"
    tests_path = processed_dir / f"stats_tests_{tag}.csv"
    descriptive(per_run).to_csv(desc_path, index=False, float_format="%.6g")
    tests = run_tests(per_run)
    tests.to_csv(tests_path, index=False, float_format="%.6g")
    print(f"Estatística descritiva: {desc_path}")
    print(f"Testes de hipótese     : {tests_path}")
    kw = tests[(tests["test"] == "kruskal_wallis") & tests["group"].str.startswith("protocolos")]
    print("\nKruskal-Wallis entre protocolos (p-valor; * <0,05, ** <0,01, *** <0,001):")
    tabela = kw.assign(p=[f"{p:.3g}{significance_stars(p)}" for p in kw["p_value"]]).pivot(
        index="group", columns="metric", values="p")
    print(tabela.reindex([f"protocolos @ {sp:g} m/s" for sp in SPEEDS]).to_string())

    print(f"\nGerando os 4 gráficos obrigatórios ({label})...")
    for m in METRICS:
        for p in plot_metric(summary, m, errorbars, figures_dir, tag, tests):
            print(f"  -> {p}")

    print(f"\nGerando boxplots de dispersão ({label})...")
    for m in METRICS:
        print(f"  -> {boxplot_metric(per_run, m, figures_dir, tag, tests)}")

    return per_run, summary


def main() -> int:
    global EXPECTED_RUNS
    ap = argparse.ArgumentParser(description="Análise dos CSVs do ns-3 (MANET): base (20n) + extensão (10n).")
    ap.add_argument("--raw", type=Path, default=ROOT / "data" / "raw" / "resultados_20n.csv",
                    help="CSV da matriz-base (20 nós)")
    ap.add_argument("--raw-ext", type=Path, default=ROOT / "data" / "raw" / "resultados_10n.csv",
                    help="CSV da extensão (10 nós); opcional — pulado se não existir")
    ap.add_argument("--processed-dir", type=Path, default=ROOT / "data" / "processed")
    ap.add_argument("--figures-dir", type=Path, default=ROOT / "figures")
    ap.add_argument("--errorbars", choices=["ci", "std"], default="ci")
    ap.add_argument("--expected-runs", type=int, default=EXPECTED_RUNS,
                    help="repetições por combinação esperadas nos CSVs (= NUM_RUNS do .cc)")
    ap.add_argument("--force", action="store_true",
                    help="gera mesmo com erros de validação (não use para o resultado final)")
    args = ap.parse_args()
    EXPECTED_RUNS = args.expected_runs

    args.processed_dir.mkdir(parents=True, exist_ok=True)
    args.figures_dir.mkdir(parents=True, exist_ok=True)

    res_20n = run_pipeline(args.raw, "20n", "20 nós (base)", args.processed_dir,
                               args.figures_dir, args.errorbars, args.force)

    res_10n = None
    if args.raw_ext.exists():
        res_10n = run_pipeline(args.raw_ext, "10n", "10 nós (extensão)", args.processed_dir,
                                   args.figures_dir, args.errorbars, args.force)
    else:
        print(f"\n(extensão pulada: {args.raw_ext} não encontrado — rode a extensão no ns-3 "
              "antes de gerar a comparação 20 vs 10 nós)")

    if res_20n is not None and res_10n is not None:
        summary_20n, summary_10n = res_20n[1], res_10n[1]
        nodes_path = args.processed_dir / "stats_nodes_comparison.csv"
        run_nodes_tests(res_20n[0], res_10n[0]).to_csv(nodes_path, index=False, float_format="%.6g")
        print(f"\nTeste 20 vs 10 nós: {nodes_path}")
        print("\nGerando comparação 20 vs 10 nós...")
        for m in METRICS:
            for p in plot_nodes_comparison(summary_20n, summary_10n, m, args.errorbars, args.figures_dir):
                print(f"  -> {p}")

    print("\nConcluído.")
    return 0 if res_20n is not None else 2


if __name__ == "__main__":
    sys.exit(main())
