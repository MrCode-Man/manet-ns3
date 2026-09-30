# manet-ns3 — Efeito da mobilidade sobre AODV, OLSR e DSDV

Trabalho Final de Redes Móveis (IFG, Câmpus Inhumas, 2026/2). Investiga como a velocidade dos nós afeta o desempenho de **AODV**, **OLSR** e **DSDV** em uma MANET simulada no **ns-3**, medindo **PDR**, **throughput**, **atraso médio** e **jitter médio**.

**Pergunta de pesquisa:** como diferentes níveis de mobilidade afetam o desempenho dos protocolos AODV, OLSR e DSDV em uma rede MANET?

## Separação de responsabilidades

| Camada | Arquivo | Faz |
|---|---|---|
| C++ / ns-3 | `src/manet-routing.cc` | monta o cenário, valida os 3 protocolos, roda as 60 execuções, calcula as métricas por fluxo e grava o CSV bruto |
| Bash | `scripts/run_batch.sh` | copia o `.cc` para o ns-3, compila, roda, e traz o CSV de volta para este projeto |
| Python | `scripts/analyze.py` | lê o CSV, valida, agrega os 4 fluxos por execução, calcula média/desvio/IC 95% e gera as tabelas e os 4 gráficos obrigatórios |

**Importante:** este projet que foi mandado aqui para o GitHub, é uma pasta **separada** da instalação do ns-3. O ns-3 (`ns-3-allinone`) é apenas a ferramenta de simulação; ele não faz parte deste repositório.

## 1. O que o enunciado define × decisões do grupo

### Definido pelo enunciado

20 nós; área 500×500 m; IEEE 802.11 Ad Hoc; `RandomWaypointMobilityModel`; UDP; 4 fluxos simultâneos; 120 s de simulação; velocidades 1/5/10/20 m/s; AODV/OLSR/DSDV; mínimo 5 repetições; seed mestre fixa (`RngSeedManager::SetSeed`) + run variável (`SetRun`); CSV com uma linha por fluxo nas colunas `protocol, speed, run, flowId, txPackets, rxPackets, lostPackets, rxBytes, throughputMbps, pdrPct, delayMs, jitterMs`.

### Decisões do grupo

| Decisão | Valor | Por quê |
|---|---|---|
| Taxa PHY 802.11b | `DsssRate11Mbps` para dados, controle **e broadcast** (`NonUnicastMode`) | Sem igualar as taxas, o broadcast (HELLO/TC/updates) viaja numa taxa mais baixa — logo com alcance maior — que os dados; o protocolo "acha" que tem rota para um vizinho que não recebe o dado de fato. Mesma escolha do exemplo oficial do ns-3 (`examples/routing/manet-routing-compare.cc`) |
| Propagação | `FriisPropagationLossModel`, 2,412 GHz | O padrão do Friis no ns-3 assume 5,15 GHz, incoerente com 802.11b (2,4 GHz), e reduz o alcance artificialmente |
| Potência de Tx | 7,5 dBm | Mesmo valor do exemplo oficial do ns-3 |
| Velocidade no RandomWaypoint | constante = velocidade da linha da matriz, para todos os nós | Torna o nível experimental (1/5/10/20 m/s) inequívoco |
| Pause time | 1,0 s, constante | — |
| Distribuição inicial | uniforme em [0,500]² | `RandomRectanglePositionAllocator` |
| Streams da mobilidade | fixados (`AssignStreams`) | Para o mesmo `run`, a topologia inicial e o movimento são idênticos nos 3 protocolos — a diferença nos resultados vem só do protocolo |
| Fluxos UDP | fluxo *i*: nó *i* → nó (19−*i*), porta 9+*i*, 1024 B a cada 0,1 s | Determinístico e reproduzível |
| Início do tráfego (warm-up) | 15 s | Um ciclo completo de atualização periódica do DSDV (15 s) e várias rodadas de HELLO/TC do OLSR (2 s/5 s); antes disso as tabelas de roteamento ainda não convergiram. O tempo total de simulação continua 120 s. O warm-up só define quando, dentro desses 120 s, o tráfego de dados começa |
| Fim do tráfego | 118 s (2 s de folga) | Dá tempo de pacotes já enviados chegarem antes do fim |
| Duração útil do throughput | `timeLastRxPacket − timeFirstTxPacket` do próprio fluxo; 0 se o fluxo não recebeu nada | Escolha simples e documentada |
| Nível de confiança (Python) | 95% | Convenção usual; o enunciado pede IC, mas não fixa o nível |
| Agregação dos 4 fluxos em 1 valor por execução (Python) | ver Seção 5 | — |

## 2. Ambiente

O ns-3 é instalado **fora** deste repositório (ex.: `~/ns-3-allinone`). Preencha com os dados da sua máquina:

| Item | Comando | Valor usado |
|---|---|---|
| Versão do ns-3 | `cd $NS3_DIR && ./ns3 --version` | _preencher_ |
| Sistema operacional | `lsb_release -a` | _preencher_ |
| Compilador | `g++ --version` | _preencher_ |
| Python | `python3 --version` | _preencher_ |

Dependências Python: `pandas`, `numpy`, `matplotlib` (opcional: `scipy`, para o IC exato; sem ele o script usa uma tabela de t de Student para 95%).

```bash
pip install pandas numpy matplotlib scipy
```

## 3. Estrutura do projeto

```text
manet-ns3/
├── README.md
├── src/
│   └── manet-routing.cc        # programa ns-3
├── scripts/
│   ├── run_batch.sh            # copia p/ ns-3, compila, roda, traz o CSV de volta
│   └── analyze.py              # validação + agregação + estatística + gráficos
├── data/
│   ├── raw/                    # resultados.csv (gerado pela simulação)
│   └── processed/              # per_run.csv, summary.csv, validation_report.txt
├── figures/                    # os 4 gráficos obrigatórios + boxplots
└── article/                    # artigo SBC
```

## 4. Compilar e rodar a simulação completa

```bash
export NS3_DIR=$HOME/ns-3-allinone      # ajuste para o caminho real do seu ns-3
cd manet-ns3
bash scripts/run_batch.sh
```

Isso sozinho copia `src/manet-routing.cc` para `$NS3_DIR/scratch/`, compila, roda a simulação inteira (validação dos 3 protocolos + as 60 execuções) e copia `resultados.csv` para `data/raw/`. Não é preciso editar nada nem passar nenhum parâmetro — tudo o que varia (protocolo, velocidade, run) já está definido dentro do `.cc`.

Se preferir rodar manualmente, os mesmos passos são:

```bash
cp src/manet-routing.cc $NS3_DIR/scratch/manet-routing.cc
cd $NS3_DIR
./ns3 build
./ns3 run scratch/manet-routing
# copie $NS3_DIR/resultado_simulacao/resultados.csv para data/raw/ deste projeto
```

### 4.1 O que aparece no console

**Etapa 1/2 — validação:** para cada protocolo, um checklist confirma que os nós foram criados, a mobilidade é `RandomWaypointMobilityModel`, o Wi-Fi está em modo Ad Hoc e o protocolo de roteamento **realmente instalado** em cada nó é o esperado. Um `[FALHA]` aqui indica um problema real de configuração — pare e investigue antes de seguir.

**Etapa 2/2 — matriz experimental:** progresso `[n/60]` e, ao final de cada execução, um resumo (`tx=... rx=... PDR=...%`) para acompanhar em tempo real.

### 4.2 Saída

`data/raw/resultados.csv`, 240 linhas de dados (60 execuções × 4 fluxos), colunas:

`protocol, speed, run, flowId, txPackets, rxPackets, lostPackets, rxBytes, throughputMbps, pdrPct, delayMs, jitterMs`

Fórmulas:

| Métrica | Fórmula | Quando é indefinida |
|---|---|---|
| PDR (%) | `100 · rx / tx` | `tx = 0` → grava `0` |
| Delay médio (ms) | `1000 · delaySum / rx` | `rx = 0` → grava `0` |
| Jitter médio (ms) | `1000 · jitterSum / (rx − 1)` | `rx ≤ 1` → grava `0` |
| Throughput (Mbps) | `rxBytes · 8 / duração / 10⁶` | `rx = 0` ou duração ≤ 0 → grava `0` |

## 5. Análise em Python

```bash
python3 scripts/analyze.py
```

### 5.1 Validação (antes de qualquer gráfico)

O script só gera tabelas e gráficos se o CSV passar na validação; caso contrário, é necessário corrigir o problema e executar novamente.

São verificadas as seguintes condições: colunas presentes; valores numéricos; protocolos ∈ {AODV, OLSR, DSDV}; velocidades ∈ {1, 5, 10, 20}; exatamente 4 linhas (fluxos) por execução; as 60 execuções completas; 5 repetições por combinação protocolo×velocidade; `rx ≤ tx`; PDR em [0,100]; `rx = 0` implica delay/jitter/throughput = 0, mantendo consistência com o `.cc`.

O relatório completo é salvo em `data/processed/validation_report.txt`.

### 5.2 Agregação dos 4 fluxos → 1 valor por execução

A mesma regra é utilizada para todas as combinações:

| Métrica | Agregação |
|---|---|
| tx, rx, lost, rxBytes | soma dos 4 fluxos |
| PDR | `100 · Σrx / Σtx` |
| Throughput | soma dos `throughputMbps` dos 4 fluxos (os fluxos concorrem na mesma janela de tempo, então a soma das vazões individuais representa a vazão agregada da execução) |
| Delay | média de `delayMs` ponderada por `rxPackets` de cada fluxo |
| Jitter | média de `jitterMs` ponderada por `max(rxPackets−1, 0)` de cada fluxo |

### 5.3 Estatística

Para cada (protocolo, velocidade), sobre as 5 repetições: média, desvio padrão amostral e IC de 95% (t de Student, 4 graus de liberdade, t ≈ 2,776). Barras de erro dos gráficos = meia-largura do IC 95% (use `--errorbars std` para desvio padrão em vez de IC).

### 5.4 Saídas

| Arquivo | Conteúdo |
|---|---|
| `data/processed/per_run.csv` | 1 linha por execução, já agregada |
| `data/processed/summary.csv` | 1 linha por (protocolo, velocidade): n, média, desvio, IC de cada métrica |
| `data/processed/validation_report.txt` | relatório da validação |
| `figures/pdr_vs_speed.{png,pdf}` | **Gráfico 1** — PDR × velocidade |
| `figures/throughput_vs_speed.{png,pdf}` | **Gráfico 2** — throughput × velocidade |
| `figures/delay_vs_speed.{png,pdf}` | **Gráfico 3** — atraso médio × velocidade |
| `figures/jitter_vs_speed.{png,pdf}` | **Gráfico 4** — jitter médio × velocidade |
| `figures/boxplot_*.png` | dispersão das 5 repetições (suplementar) |

Todos os gráficos têm eixo X e Y com unidade, legenda AODV/OLSR/DSDV e barras de erro.

## 6. Reprodutibilidade

* Seed mestre fixa (12345) e `run` = 1..5, ambos usados em `RngSeedManager::SetSeed/SetRun` a cada execução.
* `AssignStreams` garante que, para o mesmo `run`, a topologia inicial e o movimento são idênticos nos 3 protocolos — a diferença observada vem do protocolo, não da aleatoriedade da topologia.
* Rodar `bash scripts/run_batch.sh` novamente, com o mesmo `.cc`, reproduz os mesmos números.

## 7. Extensão (variável adicional)

O enunciado pede uma terceira variável experimental além de protocolo e velocidade (nº de nós, área, fluxos, taxa, pause time ou tamanho do pacote). **Ainda não implementada** — este README cobre apenas o experimento-base (3 protocolos × 4 velocidades × 5 repetições = 60 execuções). Será adicionada como uma segunda etapa, separada da matriz-base, sem alterá-la.

## 8. Solução de problemas

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| `NS3_DIR ... esta correto?` | variável não definida | `export NS3_DIR=/caminho/do/ns-3` |
| Erro de compilação | módulo do ns-3 faltando ou versão incompatível | confira se `aodv`, `olsr`, `dsdv`, `flow-monitor`, `wifi`, `mobility` estão habilitados no build |
| `analyze.py` reprova com "execuções ausentes" | a simulação não rodou as 60 execuções completas (interrompida no meio) | rode `bash scripts/run_batch.sh` de novo |
| PDR muito baixo em muitas execuções | pode ser esperado (mobilidade alta particiona a rede) ou sinal de problema de alcance do rádio | compare entre velocidades: PDR caindo conforme a velocidade sobe pode ocorrer; PDR ≈ 0% em quase tudo, mesmo a 1 m/s, é suspeito |

## 9. Referências

* Documentação do ns-3 (AODV, OLSR, DSDV, Wi-Fi, Propagation, FlowMonitor): https://www.nsnam.org/documentation/
* Exemplo oficial usado como base: `examples/routing/manet-routing-compare.cc`
* RFC 3561 (AODV); RFC 3626 (OLSR); Perkins & Bhagwat, 1994 (DSDV)
