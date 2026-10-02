# manet-ns3 — Efeito da mobilidade sobre AODV, OLSR e DSDV

Trabalho Final de Redes Móveis (IFG, Câmpus Inhumas, 2026/2). Investiga como a velocidade dos nós afeta o desempenho de **AODV**, **OLSR** e **DSDV** em uma MANET simulada no **ns-3**, medindo **PDR**, **throughput**, **atraso médio** e **jitter médio**.

**Pergunta de pesquisa:** como diferentes níveis de mobilidade afetam o desempenho dos protocolos AODV, OLSR e DSDV em uma rede MANET?

## Separação de responsabilidades

| Camada | Arquivo | Faz |
|---|---|---|
| C++ / ns-3 | `src/manet-routing.cc` | monta o cenário, valida os 3 protocolos, roda os dois estágios (base 20 nós + extensão 10 nós), calcula as métricas por fluxo e grava um CSV por estágio, uma amostra de XML do FlowMonitor e o log completo da execução |
| Bash | `scripts/run_batch.sh` | cria um link simbólico do `.cc` dentro do ns-3, compila, roda, e traz os dois CSVs, o log e o XML de volta para este projeto |
| Python | `scripts/analyze.py` | lê os dois CSVs, valida, agrega os 4 fluxos por execução, calcula média/desvio/IC 95%, gera as tabelas e os 4 gráficos obrigatórios por estágio, e uma comparação 20 vs 10 nós |

**Importante:** este projet que foi mandado aqui para o GitHub, é uma pasta **separada** da instalação do ns-3. O ns-3 (`ns-3-allinone`) é apenas a ferramenta de simulação; ele não faz parte deste repositório.

## 1. O que o enunciado define × decisões do grupo

### Definido pelo enunciado

20 nós; área 500×500 m; IEEE 802.11 Ad Hoc; `RandomWaypointMobilityModel`; UDP; 4 fluxos simultâneos; 120 s de simulação; velocidades 1/5/10/20 m/s; AODV/OLSR/DSDV; mínimo de 5 repetições; seed mestre fixa (`RngSeedManager::SetSeed`) + run variável (`SetRun`); CSV com uma linha por fluxo nas colunas `protocol, speed, run, flowId, txPackets, rxPackets, lostPackets, rxBytes, throughputMbps, pdrPct, delayMs, jitterMs`.

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
| Repetições por combinação | **25** (acima do mínimo de 5 exigido pelo enunciado) | Maior robustez estatística: médias e IC 95% mais estáveis entre protocolo×velocidade |
| Escopo do XML do FlowMonitor | amostra: 1 arquivo por combinação protocolo×velocidade×estágio (`run = 1`), 24 arquivos no total — não as 600 execuções | Exportar o XML completo pesaria muito no repositório e deixaria a simulação mais lenta; a amostra já demonstra o uso do `SerializeToXmlFile` e serve de referência detalhada por combinação |
| Variável de extensão | número de nós: **20** (base) vs **10** (extensão) — ver Seção 7 | Citada como exemplo válido pelo enunciado; reduzir os nós pela metade na mesma área muda diretamente a densidade/conectividade da rede, afetando como os protocolos descobrem e mantêm rotas |
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
│   └── manet-routing.cc        # programa ns-3 (base 20n + extensão 10n)
├── scripts/
│   ├── run_batch.sh            # linka p/ ns-3, compila, roda, traz os CSVs de volta
│   └── analyze.py              # validação + agregação + estatística + gráficos (por estágio + comparação)
├── data/
│   ├── raw/                    # resultados_20n.csv, resultados_10n.csv, log.txt, flowmon/
│   │   └── flowmon/             # amostra de XML do FlowMonitor (1 por protocolo×velocidade×estágio)
│   └── processed/              # per_run_<tag>.csv, summary_<tag>.csv, validation_report_<tag>.txt
├── figures/                    # 4 gráficos obrigatórios + boxplots por estágio, + 4 de comparação 20n/10n
└── article/                    # artigo SBC
```

## 4. Compilar e rodar a simulação completa (base + extensão)

```bash
export NS3_DIR=$HOME/ns-3-allinone      # ajuste para o caminho real do seu ns-3
cd manet-ns3
bash scripts/run_batch.sh
```

Isso sozinho cria o link simbólico de `src/manet-routing.cc` em `$NS3_DIR/scratch/`, compila, roda a simulação inteira — validação dos 3 protocolos + matriz-base (300 execuções, 20 nós) + extensão (300 execuções, 10 nós), 600 execuções no total — e copia `resultados_20n.csv` e `resultados_10n.csv` para `data/raw/`. Não é preciso editar nada nem passar nenhum parâmetro — tudo o que varia (protocolo, velocidade, run, nº de nós) já está definido dentro do `.cc`.

Se preferir rodar manualmente, os mesmos passos são:

```bash
ln -s "$(pwd)/src/manet-routing.cc" $NS3_DIR/scratch/manet-routing.cc
cd $NS3_DIR
./ns3 build
./ns3 run scratch/manet-routing
# copie $NS3_DIR/resultado_simulacao/resultados_20n.csv e resultados_10n.csv para data/raw/ deste projeto
```

### 4.1 Execução de um cenário único

O programa **não aceita parâmetros de linha de comando** — o `CommandLine` em `main()` não registra nenhum `cmd.AddValue`, então `./ns3 run scratch/manet-routing` sempre executa os dois estágios inteiros (3 protocolos × 4 velocidades × 25 repetições = 300 execuções, a 20 nós e de novo a 10 nós) de uma vez; não há como rodar isoladamente uma única combinação a partir da linha de comando.

Para testar rapidamente um único caso (ex.: durante o desenvolvimento), o caminho é editar temporariamente as constantes `PROTOCOLS`, `SPEEDS`, `NUM_RUNS` e/ou a lista `STAGES` no topo de `src/manet-routing.cc` para conter só o que se quer testar, recompilar (`./ns3 build`) e rodar — desfazendo a alteração antes de gerar os dados finais da matriz completa.

### 4.2 O que aparece no console (e é gravado em log.txt)

**Etapa 1/3 — validação:** para cada protocolo, um checklist confirma que os nós foram criados, a mobilidade é `RandomWaypointMobilityModel`, o Wi-Fi está em modo Ad Hoc e o protocolo de roteamento **realmente instalado** em cada nó é o esperado. Um `[FALHA]` aqui indica um problema real de configuração — pare e investigue antes de seguir.

**Etapa 2/3 — matriz-base (20 nós)** e **Etapa 3/3 — extensão (10 nós):** progresso `[n/300]` (reiniciado a cada estágio) e, ao final de cada execução, um resumo com o estágio entre colchetes (`[20n] ... PDR=...%` / `[10n] ... PDR=...%`) para acompanhar em tempo real.

Tudo isso é espelhado automaticamente em `data/raw/log.txt` (um `std::streambuf` duplica cada escrita em `std::cout` também para o arquivo, instalado uma vez no início de `main()`) — o log em arquivo é idêntico ao que aparece no terminal, sem precisar copiar/colar nada manualmente.

### 4.3 Saída

Um CSV por estágio, cada um com 1200 linhas de dados (300 execuções × 4 fluxos):

| Estágio | Arquivo | Nós |
|---|---|---|
| Base | `data/raw/resultados_20n.csv` | 20 |
| Extensão | `data/raw/resultados_10n.csv` | 10 |

Colunas (iguais nos dois arquivos — o número de nós fica implícito no nome do arquivo, não é uma coluna):

`protocol, speed, run, flowId, txPackets, rxPackets, lostPackets, rxBytes, throughputMbps, pdrPct, delayMs, jitterMs`

Fórmulas:

| Métrica | Fórmula | Quando é indefinida |
|---|---|---|
| PDR (%) | `100 · rx / tx` | `tx = 0` → grava `0` |
| Delay médio (ms) | `1000 · delaySum / rx` | `rx = 0` → grava `0` |
| Jitter médio (ms) | `1000 · jitterSum / (rx − 1)` | `rx ≤ 1` → grava `0` |
| Throughput (Mbps) | `rxBytes · 8 / duração / 10⁶` | `rx = 0` ou duração ≤ 0 → grava `0` |

Além dos CSVs, a simulação grava mais dois tipos de saída em `data/raw/`:

| Saída | Caminho | Conteúdo |
|---|---|---|
| Log completo | `data/raw/log.txt` | Cópia em arquivo de tudo que aparece no console (Seção 4.2), os dois estágios |
| XML do FlowMonitor (amostra) | `data/raw/flowmon/flowmon_<protocolo>_speed<velocidade>_<tag>.xml` | 24 arquivos (`<tag>` = `20n` ou `10n`) — `SerializeToXmlFile` chamado só para `run = 1` de cada combinação protocolo×velocidade×estágio (ver "Decisões do grupo", Seção 1, para a justificativa de não exportar as 600 execuções) |

## 5. Análise em Python

```bash
python3 scripts/analyze.py
```

Por padrão processa os dois estágios: `data/raw/resultados_20n.csv` (base) e `data/raw/resultados_10n.csv` (extensão). A extensão é opcional — se `resultados_10n.csv` ainda não existir, o script analisa só a base e avisa que a comparação 20 vs 10 nós foi pulada (não é tratado como erro). Para apontar para outros caminhos: `--raw outro20n.csv --raw-ext outro10n.csv`.

### 5.1 Validação (antes de qualquer gráfico)

O script só gera tabelas e gráficos de um estágio se o CSV correspondente passar na validação; caso contrário, é necessário corrigir o problema e executar novamente. Os dois estágios são validados de forma independente (a base pode ter sucesso mesmo que a extensão falhe, e vice-versa).

São verificadas as seguintes condições, em cada CSV: colunas presentes; valores numéricos; protocolos ∈ {AODV, OLSR, DSDV}; velocidades ∈ {1, 5, 10, 20}; exatamente 4 linhas (fluxos) por execução; as 300 execuções completas; 25 repetições por combinação protocolo×velocidade; `rx ≤ tx`; PDR em [0,100]; `rx = 0` implica delay/jitter/throughput = 0, mantendo consistência com o `.cc`.

O relatório completo de cada estágio é salvo em `data/processed/validation_report_<tag>.txt` (`<tag>` = `20n` ou `10n`).

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

Para cada (protocolo, velocidade), em cada estágio, sobre as 25 repetições: média, desvio padrão amostral e IC de 95% (t de Student, 24 graus de liberdade). Com `scipy` instalado, o t crítico exato é calculado (`stats.t.ppf`, t ≈ 2,064); sem `scipy`, o script usa uma tabela de fallback que só cobre g.l. de 1 a 9 — para 24 g.l. ela cai no valor padrão `1,960` (aproximação normal), levemente mais otimista que o t exato. Para IC preciso com 25 repetições, instale `scipy`. Barras de erro dos gráficos = meia-largura do IC 95% (use `--errorbars std` para desvio padrão em vez de IC).

### 5.4 Saídas

Por estágio (`<tag>` = `20n` ou `10n`):

| Arquivo | Conteúdo |
|---|---|
| `data/processed/per_run_<tag>.csv` | 1 linha por execução, já agregada |
| `data/processed/summary_<tag>.csv` | 1 linha por (protocolo, velocidade): n, média, desvio, IC de cada métrica |
| `data/processed/validation_report_<tag>.txt` | relatório da validação |
| `figures/pdr_vs_speed_<tag>.{png,pdf}` | **Gráfico 1** — PDR × velocidade |
| `figures/throughput_vs_speed_<tag>.{png,pdf}` | **Gráfico 2** — throughput × velocidade |
| `figures/delay_vs_speed_<tag>.{png,pdf}` | **Gráfico 3** — atraso médio × velocidade |
| `figures/jitter_vs_speed_<tag>.{png,pdf}` | **Gráfico 4** — jitter médio × velocidade |
| `figures/boxplot_<metrica>_<tag>.png` | dispersão das 25 repetições (suplementar) |

Comparação entre estágios (só gerada se os dois CSVs existirem e passarem na validação):

| Arquivo | Conteúdo |
|---|---|
| `figures/pdr_nodes_comparison.{png,pdf}` | PDR × velocidade, 20 vs 10 nós (6 séries: 3 protocolos × 2 contagens de nós) |
| `figures/throughput_nodes_comparison.{png,pdf}` | idem para throughput |
| `figures/delay_nodes_comparison.{png,pdf}` | idem para atraso médio |
| `figures/jitter_nodes_comparison.{png,pdf}` | idem para jitter |

Todos os gráficos têm eixo X e Y com unidade, legenda com protocolo (e, nos de comparação, nº de nós) e barras de erro.

## 6. Reprodutibilidade

* Seed mestre fixa (12345) e `run` = 1..25, ambos usados em `RngSeedManager::SetSeed/SetRun` a cada execução — nos dois estágios (20n e 10n).
* `AssignStreams` garante que, para o mesmo `run`, a topologia inicial e o movimento são idênticos nos 3 protocolos — a diferença observada vem do protocolo, não da aleatoriedade da topologia (dentro de um mesmo estágio; a topologia em si muda entre 20n e 10n, pois o número de nós é justamente a variável manipulada).
* Rodar `bash scripts/run_batch.sh` novamente, com o mesmo `.cc`, reproduz os mesmos números nos dois estágios.

### 6.1 Rastreabilidade

**Commit da versão entregue:** _preencher ao fazer o commit final, antes do envio de 15/out_ — registre aqui o hash (`git rev-parse HEAD`) do commit que corresponde exatamente ao código, dados e artigo enviados no Moodle.

## 7. Extensão: número de nós (20 vs 10)

O enunciado pede uma terceira variável experimental além de protocolo e velocidade (nº de nós, área, fluxos, taxa, pause time ou tamanho do pacote). O grupo escolheu **número de nós**: a matriz-base usa 20 nós; a extensão repete a **mesma matriz completa** (3 protocolos × 4 velocidades × 25 repetições = 300 execuções) com **10 nós**, na mesma área de 500×500 m — ou seja, a extensão também reduz a densidade da rede, o que é esperado afetar a conectividade e, por consequência, o desempenho de cada protocolo de roteamento.

**Separação da matriz-base:** a extensão é um segundo estágio independente dentro do mesmo programa (`src/manet-routing.cc`, `std::vector<Stage> STAGES`), com seu próprio CSV (`resultados_10n.csv`), sua própria amostra de XML e suas próprias linhas de progresso no log — nada na matriz-base (`resultados_20n.csv`, já existente) é alterado ou misturado com os dados da extensão.

**Como rodar:** `bash scripts/run_batch.sh` já roda os dois estágios em sequência (não há como rodar só um dos dois sem editar o `.cc` — ver Seção 4.1). **Como analisar:** `python3 scripts/analyze.py` já processa os dois CSVs e, se ambos existirem e passarem na validação, gera automaticamente 4 gráficos comparando 20 vs 10 nós (Seção 5.4).

**Hipótese:** com menos nós na mesma área, a rede fica menos densa — menos vizinhos por nó, rotas mais longas (mais saltos) ou até partições da rede em subgrupos desconectados. Espera-se que isso afete PDR e delay de forma diferente conforme a estratégia de cada protocolo (reativo vs proativo, Seção 1) — essa comparação é o que os gráficos `*_nodes_comparison.*` (Seção 5.4) permitem visualizar, e deve ser discutida no artigo (`article/`, ver nota ao final da Seção 9).

## 8. Solução de problemas

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| `NS3_DIR ... esta correto?` | variável não definida | `export NS3_DIR=/caminho/do/ns-3` |
| Erro de compilação | módulo do ns-3 faltando ou versão incompatível | confira se `aodv`, `olsr`, `dsdv`, `flow-monitor`, `wifi`, `mobility` estão habilitados no build |
| `analyze.py` reprova com "execuções ausentes" | a simulação não rodou as 300 execuções completas de algum estágio (interrompida no meio, ou os 2 estágios de 600 execuções ainda não terminaram) | rode `bash scripts/run_batch.sh` de novo (ou espere terminar — são 600 execuções no total, 2x o tempo de rodar só a base) |
| PDR muito baixo em muitas execuções | pode ser esperado (mobilidade alta particiona a rede) ou sinal de problema de alcance do rádio | compare entre velocidades: PDR caindo conforme a velocidade sobe pode ocorrer; PDR ≈ 0% em quase tudo, mesmo a 1 m/s, é suspeito |

## 9. Tutorial de reprodução completa (do zero à rastreabilidade)

Esta seção é o caminho único, em ordem, para reproduzir o experimento inteiro partindo de uma máquina sem nada instalado — a mesma sequência do checklist de reprodução do enunciado: ambiente → compilação → execução → parâmetros → aleatoriedade → saídas → análise → rastreabilidade. Cada passo tem o comando real; os detalhes e justificativas completas ficam nas seções já referenciadas, para não duplicar conteúdo.

### 9.1 Instalar o ns-3 (ambiente)

Dependências mínimas (Ubuntu/Debian — adapte para sua distro):

```bash
sudo apt update
sudo apt install -y g++ python3 cmake ninja-build git
```

Baixe e compile o ns-3 (troque `<VERSAO>` pela versão que o grupo vai usar — confira a mais recente em https://www.nsnam.org/releases/):

```bash
cd ~
wget https://www.nsnam.org/releases/ns-allinone-<VERSAO>.tar.bz2
tar xjf ns-allinone-<VERSAO>.tar.bz2
cd ns-allinone-<VERSAO>/ns-<VERSAO>
./ns3 configure --enable-examples --enable-tests
./ns3 build
./ns3 run hello-simulator      # confirma que a instalação funcionou
```

Anote a versão do ns-3, o SO, o compilador e a versão do Python usados aqui — são exatamente os itens da tabela da Seção 2 (Ambiente), que deve ser preenchida com esses valores.

### 9.2 Clonar este repositório

```bash
git clone <URL deste repositório> manet-ns3
cd manet-ns3
export NS3_DIR=~/ns-allinone-<VERSAO>/ns-<VERSAO>   # pasta onde está o script ./ns3
pip install pandas numpy matplotlib scipy            # dependências Python (Seção 2)
```

### 9.3 Compilar e rodar (compilação + execução)

```bash
bash scripts/run_batch.sh
```

Isso cria o link do `.cc` dentro do ns-3, compila (`./ns3 build`) e roda os dois estágios completos — matriz-base (300 execuções, 20 nós) e extensão (300 execuções, 10 nós), 600 no total — ver Seção 4 para os comandos manuais equivalentes, e a Seção 4.1 para como testar um único cenário (o programa não aceita parâmetros de linha de comando, então "um cenário" exige editar constantes no `.cc`, como explicado lá).

### 9.4 Parâmetros usados

Nenhum parâmetro é passado em tempo de execução — tudo o que varia (protocolo, velocidade, run, nº de nós) já está fixado dentro do `.cc`. O significado, a unidade e o valor de cada parâmetro (os definidos pelo enunciado, os decididos pelo grupo, e a variável de extensão nº de nós) estão nas Seções 1 e 7.

### 9.5 Aleatoriedade e política de repetição

Seed mestre fixa (`12345`) e `run` de 1 a 25 — uma repetição independente por `run`, mesma seed mestre, nos dois estágios (Seção 6). Rodar `bash scripts/run_batch.sh` de novo, sem alterar o `.cc`, reproduz exatamente os mesmos números.

### 9.6 Onde as saídas ficam

`data/raw/resultados_20n.csv` e `data/raw/resultados_10n.csv` (1200 linhas cada, 300 execuções × 4 fluxos) têm os dados completos da matriz-base e da extensão, respectivamente. Além deles, `data/raw/log.txt` tem o log completo da execução (espelho do console, os dois estágios) e `data/raw/flowmon/` tem uma amostra de 24 XMLs do FlowMonitor — 1 por combinação protocolo×velocidade×estágio, `run = 1` (detalhes na Seção 4.3).

### 9.7 Gerar tabelas, estatística e gráficos (análise)

```bash
python3 scripts/analyze.py
```

Gera, para cada estágio, `data/processed/per_run_<tag>.csv`, `summary_<tag>.csv`, `validation_report_<tag>.txt` e os 4 gráficos obrigatórios + boxplots em `figures/` (`<tag>` = `20n`/`10n`); e, se os dois estágios tiverem dados válidos, mais 4 gráficos comparando 20 vs 10 nós (detalhes e fórmulas na Seção 5).

### 9.8 Rastreabilidade

Depois de confirmar que tudo rodou e os gráficos saíram sem erro de validação, faça o commit final (código + dados + artigo juntos) e registre o hash na Seção 6.1:

```bash
git add -A
git commit -m "versão entregue"
git rev-parse HEAD      # copie esse hash para a Seção 6.1
```

### 9.9 Teste decisivo

Clone o repositório em um diretório **novo** (fora deste) e repita os passos 9.1 a 9.8 do zero, sem nenhum ajuste além de `NS3_DIR` e `<VERSAO>`. Se os 4 gráficos obrigatórios de cada estágio (20n e 10n) e os 4 gráficos de comparação saírem sem erro de validação, a reprodução funcionou — esse é o critério do enunciado para considerar o trabalho reproduzível.

> **Nota:** a pasta `article/` ainda não existe neste repositório — o artigo em SBC (até 5 páginas), com o link do repositório destacado no corpo do texto, ainda precisa ser escrito e adicionado antes da entrega (15/out).

## 10. Referências

* Documentação do ns-3 (AODV, OLSR, DSDV, Wi-Fi, Propagation, FlowMonitor): https://www.nsnam.org/documentation/
* Exemplo oficial usado como base: `examples/routing/manet-routing-compare.cc`
* RFC 3561 (AODV); RFC 3626 (OLSR); Perkins & Bhagwat, 1994 (DSDV)
