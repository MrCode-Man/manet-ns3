# Documentação Técnica Completa — Simulação MANET (AODV, OLSR, DSDV)

> Documento de referência/apresentação, sem limite de páginas — cobre tudo que foi construído, investigado e observado neste repositório. Para a versão formal e enxuta (5 páginas, formato SBC, só os achados centrais), ver `artigo.tex`/`artigo.pdf` nesta mesma pasta.

**Autores:** Guilherme Alixandre Gonçalves Silva, Gabriel Araujo de Ataides, Victor Inacio
**Instituição:** IFG — Campus Inhumas, Redes Móveis (2026/2)

---

## Sumário

1. [Visão geral do projeto](#1-visão-geral-do-projeto)
2. [Cenário de simulação](#2-cenário-de-simulação)
3. [Métodos de transmissão (camada física e de enlace)](#3-métodos-de-transmissão-camada-física-e-de-enlace)
4. [Os três protocolos de roteamento](#4-os-três-protocolos-de-roteamento)
5. [Métricas e como são calculadas](#5-métricas-e-como-são-calculadas)
6. [Arquitetura do projeto (pipeline)](#6-arquitetura-do-projeto-pipeline)
7. [Metodologia estatística](#7-metodologia-estatística)
8. [Reprodutibilidade: o bug de RNG encontrado e corrigido](#8-reprodutibilidade-o-bug-de-rng-encontrado-e-corrigido)
9. [Principais resultados e observações](#9-principais-resultados-e-observações)
10. [Limitações e perguntas em aberto](#10-limitações-e-perguntas-em-aberto)
11. [Mapa do repositório](#11-mapa-do-repositório)

---

## 1. Visão geral do projeto

Este trabalho compara três protocolos de roteamento para MANET (Mobile Ad hoc Network) — **AODV**, **OLSR** e **DSDV** — simulados no **ns-3**, medindo como cada um se comporta sob mobilidade variável (1 a 20 m/s) e sob duas densidades de nós (20 e 10 nós na mesma área, a variável de extensão exigida pelo enunciado da disciplina).

**Pergunta de pesquisa:** como a mobilidade e a densidade de nós afetam o desempenho de AODV, OLSR e DSDV numa MANET?

A resposta não é simples nem única — é o que este documento detalha. Resumidamente: não existe um protocolo "melhor" em todas as métricas; cada um troca confiabilidade por previsibilidade de forma diferente, e a densidade de nós prejudica ou beneficia cada protocolo por um mecanismo interno distinto, não de forma uniforme.

---

## 2. Cenário de simulação

| Parâmetro | Valor |
|---|---|
| Simulador | ns-3 |
| Rede | Wi-Fi 802.11b, modo *ad hoc* (sem ponto de acesso) |
| Área | 500 × 500 m, quadrada |
| Número de nós | 20 (matriz-base) e 10 (extensão) — mesma área nos dois casos |
| Mobilidade | RandomWaypoint: velocidade constante por rodada (1, 5, 10 ou 20 m/s, igual para todos os nós), pausa de 1 s em cada waypoint, posição inicial uniforme |
| Tráfego | 4 fluxos UDP simultâneos e determinísticos (fluxo *i* liga o nó *i* ao nó `N-1-i`), pacotes de 1024 bytes a cada 0,1 s |
| Janela de tráfego | de 15 s a 118 s, dentro de uma simulação de 120 s |
| Protocolos | AODV, OLSR, DSDV |
| Repetições | 60 por combinação (protocolo × velocidade × densidade) — 1440 execuções no total |
| Seed | Mestre fixa (12345) + número de execução (`run`) variável — reprodutível e, ao mesmo tempo, amostras independentes |

### Por que a janela de tráfego começa em 15 s, não em 0 s?

Os protocolos proativos (OLSR, DSDV) precisam de tempo para construir suas tabelas de roteamento antes que o tráfego de dados comece, senão a medição capturaria principalmente o período de convergência, não o regime estável:

- **DSDV** tem um ciclo completo de atualização periódica de 15 s — por isso o warm-up é exatamente esse valor.
- **OLSR** usa HELLO a cada 2 s e TC (Topology Control) a cada 5 s — várias rodadas já acontecem dentro dos 15 s.
- **AODV**, sendo reativo, não precisa desse tempo (não tem tabela para convergir antecipadamente) — mas herda o mesmo warm-up porque o tráfego é compartilhado entre os três protocolos pela mesma estrutura de simulação.

O tráfego para em 118 s (2 s antes do fim) para dar tempo de pacotes já enviados chegarem antes do FlowMonitor parar de contar.

---

## 3. Métodos de transmissão (camada física e de enlace)

| Parâmetro | Valor | Por quê |
|---|---|---|
| Padrão | 802.11b (DSSS) | Especificado pelo enunciado |
| Taxa de dados | `DsssRate11Mbps` (11 Mbps nominal) | — |
| Taxa de controle e **broadcast** | Igualada à taxa de dados (`NonUnicastMode`) | Se o broadcast (HELLO, TC, updates de roteamento) usasse a taxa básica padrão — mais baixa, logo com alcance maior que os dados — o protocolo "enxergaria" vizinhos que, na prática, não conseguem receber dados na taxa configurada. O resultado seria uma rota "visível" na tabela que não entrega pacote de verdade. |
| Potência de transmissão | 7,5 dBm (~5,6 mW), fixa em todas as execuções | Mesmo valor do exemplo oficial do ns-3 (`examples/routing/manet-routing-compare.cc`) |
| Modelo de propagação | `FriisPropagationLossModel`, 2,412 GHz (canal 1 do 2,4 GHz) | O padrão do Friis no ns-3 assume 5,15 GHz — incoerente com 802.11b, reduziria o alcance artificialmente |
| Alcance de decodificação estimado | ~300 m (não medido diretamente no simulador) | Comparável ao tamanho da área (500×500 m) — muitos pares de nós ficam "na beirada" do alcance, o que já é suficiente para explicar perdas sem precisar de congestionamento (ver Seção 9) |
| Transporte | UDP (sem retransmissão em nível de transporte) | Perdas refletem só a camada de roteamento/enlace, não retransmissão TCP |

**Carga oferecida vs. capacidade:** 4 fluxos × 1024 B a cada 0,1 s ≈ 0,0842 Mbps por fluxo, 0,337 Mbps no total — contra 11 Mbps nominais do 802.11b (~5-6 Mbps úteis). O canal é usado a apenas ~3-6% da capacidade. Isso é importante: as perdas observadas (25-45% dos pacotes, dependendo do protocolo/velocidade) **não vêm de congestionamento** — vêm de problemas de conectividade/roteamento sob mobilidade, não de saturação do meio.

---

## 4. Os três protocolos de roteamento

### 4.1 AODV (*Ad hoc On-Demand Distance Vector*) — reativo

**Como funciona:** o AODV não mantém rotas prontas o tempo todo. Quando um nó precisa enviar um pacote para um destino sem rota conhecida, ele dispara um **RREQ** (*Route Request*) em broadcast, que se propaga pela rede (inundação controlada por TTL/número de sequência). Quando o RREQ chega ao destino (ou a um nó que já tem rota válida para ele), um **RREP** (*Route Reply*) é enviado de volta em unicast, estabelecendo a rota. Números de sequência garantem que a rota mais recente seja usada, evitando loops. Se um enlace da rota quebra, um **RERR** (*Route Error*) notifica os nós afetados, que descobrem uma rota nova sob demanda.

**Ponto crítico para este trabalho:** enquanto a rota está sendo descoberta (RREQ/RREP em trânsito), o AODV **enfileira** o pacote de dados em vez de descartá-lo — e o entrega assim que a rota é estabelecida, mesmo que isso demore. É esse comportamento que explica por que o AODV tem atraso e jitter sistematicamente maiores que os outros dois protocolos (Seção 9).

### 4.2 OLSR (*Optimized Link State Routing*) — proativo

**Como funciona:** cada nó mantém uma tabela de roteamento completa, atualizada continuamente através de dois tipos de mensagem periódica:
- **HELLO** (a cada 2 s): trocada só entre vizinhos diretos, serve para descoberta de vizinhança e, principalmente, para cada nó eleger seus **MPR** (*Multipoint Relays*) — um subconjunto mínimo de vizinhos que, se retransmitirem uma mensagem, alcançam todos os vizinhos-de-vizinhos do nó.
- **TC** (*Topology Control*, a cada 5 s): anuncia a vizinhança do nó para o resto da rede, mas **só os MPRs retransmitem** essa mensagem — não todo mundo. É essa otimização (dar nome ao "O" de OLSR) que reduz drasticamente o overhead de inundação comparado a um link-state ingênuo.

Como cada nó já tem uma rota calculada (via caminho mais curto sobre a topologia conhecida através dos MPRs), não há fila de espera por descoberta: se não há rota válida no instante do envio, o pacote é descartado ali mesmo.

### 4.3 DSDV (*Destination-Sequenced Distance-Vector*) — proativo

**Como funciona:** cada nó mantém uma tabela de roteamento (destino, próximo salto, número de saltos, número de sequência) e a retransmite **inteira** para seus vizinhos periodicamente (no nosso cenário, ciclo de 15 s) ou de forma disparada quando uma mudança significativa de topologia é detectada. Números de sequência (sempre pares para anúncios normais, ímpares para anunciar uma rota quebrada) evitam loops de roteamento, um problema clássico de protocolos de vetor-distância puro (contagem ao infinito).

**Ponto crítico para este trabalho:** diferente do OLSR, o DSDV **não** usa uma estrutura como o MPR para limitar o overhead de inundação — cada atualização é um broadcast de alcance total da tabela, cujo **tamanho cresce com o número total de nós da rede** (uma entrada por destino conhecido). Isso explica por que o DSDV é o protocolo mais sensível à densidade de nós (Seção 9): com mais nós, os broadcasts ficam maiores e mais frequentes na rede como um todo, aumentando colisões no meio compartilhado.

### Resumo comparativo

| | AODV | OLSR | DSDV |
|---|---|---|---|
| Classe | Reativo | Proativo | Proativo |
| Quando descobre rota | Sob demanda | Continuamente | Continuamente |
| O que faz sem rota válida | Enfileira o pacote e descobre | N/A (rota já existe ou não) | N/A (rota já existe ou não) |
| Overhead de controle | Só quando precisa (RREQ/RREP/RERR) | Periódico, mas limitado por MPR | Periódico, tabela inteira |
| Overhead escala com | Diâmetro da rede (saltos até o destino) | Vizinhança local (grau do nó) | Número total de nós (tamanho da tabela) |
| Robustez à mobilidade | Alta (recalcula sob demanda) | Média-alta (ciclo rápido de HELLO/TC) | Baixa-média (ciclo de 15s pode ficar obsoleto) |
| Robustez à densidade | Boa em PDR, ruim em atraso/jitter | Boa na maioria das métricas | Ruim — overhead cresce com N |

---

## 5. Métricas e como são calculadas

Todas as métricas vêm do **FlowMonitor** do ns-3, por fluxo, e são agregadas por execução (soma/média ponderada dos 4 fluxos — ver `scripts/analyze.py`).

| Métrica | Fórmula | Observação importante |
|---|---|---|
| **PDR** (Packet Delivery Ratio) | `100 × rxPackets / txPackets` | `txPackets` é contado na camada de aplicação (UdpClient), não no FlowMonitor — isso evita que um pacote descartado antes de entrar no FlowMonitor (comum no OLSR, que não enfileira) desapareça da contagem sem entrar como perda |
| **Throughput** | `rxBytes × 8 / duração_útil / 10⁶` Mbps | A duração útil é `timeLastRxPacket − timeFirstTxPacket` do próprio fluxo — ou seja, mede a vazão só **enquanto o fluxo esteve ativo**, não na janela de 103 s inteira. Isso pode inflar artificialmente o throughput de fluxos intermitentes (poucos pacotes, período ativo curto) |
| **Atraso médio** | `1000 × delaySum / rxPackets` ms | Calculado **só sobre pacotes recebidos** — um pacote perdido não tem "atraso", literalmente não existe timestamp de chegada para ele |
| **Jitter médio** | `1000 × jitterSum / (rxPackets − 1)` ms | Mesma limitação do atraso: só pacotes recebidos. Indefinido se `rxPackets ≤ 1` |

### A limitação mais importante: viés de sobrevivência

Atraso e jitter **só contam pacotes que chegaram**. Isso parece óbvio, mas tem uma consequência central para interpretar os resultados: o AODV enfileira pacotes "difíceis" (sem rota no momento) e eventualmente os entrega, atrasados — esses pacotes entram na média de atraso/jitter do AODV, puxando-a pra cima. OLSR e DSDV simplesmente descartam esses mesmos pacotes difíceis — eles nunca entram na conta de atraso/jitter, só reduzem o PDR. Comparar atraso entre protocolos com PDRs muito diferentes não é "maçã com maçã": o protocolo que "parece" mais rápido pode só estar medindo os pacotes fáceis.

---

## 6. Arquitetura do projeto (pipeline)

```
src/manet-routing.cc  →  scripts/run_batch.sh  →  scripts/analyze.py
   (simula no ns-3)        (orquestra/paraleliza)      (estatística/gráficos)
```

| Camada | Arquivo | Função |
|---|---|---|
| C++ / ns-3 | `src/manet-routing.cc` | Monta o cenário, valida os 3 protocolos, roda a matriz experimental completa (ou uma fatia dela, via `--protocol/--stage/--speed/--outDir`), calcula métricas por fluxo e grava CSV + log + amostra de XML do FlowMonitor |
| Bash | `scripts/run_batch.sh` | Faz backup das saídas anteriores, liga o `.cc` ao ns-3 via symlink, compila, roda a simulação (em paralelo, por padrão — `JOBS` processos simultâneos, um por combinação estágio×protocolo×velocidade), junta os resultados e chama a análise |
| Python | `scripts/analyze.py` | Valida os CSVs, agrega os 4 fluxos por execução, calcula estatística descritiva e testes de hipótese, gera tabelas processadas e todos os gráficos |

**Execução paralela:** como cada combinação (protocolo, velocidade, run) é uma execução independente e isolada (graças ao fix de reprodutibilidade — Seção 8), o `run_batch.sh` divide a matriz em 24 partes (2 estágios × 3 protocolos × 4 velocidades) e roda com `xargs -P JOBS`, juntando os CSVs ao final na mesma ordem de uma rodada sequencial. Isso reduziu o tempo da matriz completa de ~79 min (sequencial) para ~30 min (paralelo, 10 processos), num notebook de 12 threads.

---

## 7. Metodologia estatística

As distribuições de atraso e jitter têm cauda longa (confirmado por teste de **Shapiro-Wilk** em cada célula protocolo×velocidade×densidade — a maioria rejeita normalidade), então o pipeline usa testes **não paramétricos**, mais apropriados que média/ANOVA/teste-t para esse tipo de dado:

| Teste | Para quê |
|---|---|
| **Shapiro-Wilk** | Checar normalidade de cada célula (diagnóstico, não decide nada sozinho) |
| **Levene** | Checar homogeneidade de variância entre protocolos, por velocidade |
| **Kruskal-Wallis** | Testar se há diferença entre os 3 protocolos (por velocidade) ou entre as 4 velocidades (por protocolo) — é o equivalente não paramétrico da ANOVA de um fator |
| **Mann-Whitney** | Comparação par-a-par (entre 2 protocolos, ou entre 2 densidades) |
| **Correção de Holm** | Ajusta o limiar de significância quando várias comparações são feitas ao mesmo tempo (controla a chance de "achar" uma diferença falsa só por acaso, testando muitas hipóteses) |
| **Delta de Cliff** | Tamanho de efeito não paramétrico — complementa o p-valor dizendo *o quanto* a diferença importa, não só se ela existe |

Médias e IC95% (via distribuição *t* de Student) continuam sendo reportados, mas só como **resumo descritivo** — a decisão sobre "essa diferença é real?" vem dos testes acima, não de olhar duas médias e seus intervalos de erro a olho nu (ver Seção 9 para por que isso importa).

---

## 8. Reprodutibilidade: o bug de RNG encontrado e corrigido

Esta foi uma das descobertas mais importantes do projeto, documentada em detalhe porque afeta a validade de qualquer conclusão tirada dos dados.

### O sintoma

Rodando a **mesma configuração** (mesmo protocolo, velocidade, densidade e número de `run`) duas vezes dentro do mesmo processo, o resultado **mudava** — por exemplo, OLSR a 10 nós, 1 m/s, run 33, deu 50% de PDR numa vez e 0% noutra.

### A causa

O ns-3 atribui números de stream (sementes de sub-geradores aleatórios) automaticamente a qualquer variável aleatória que não receba um stream explícito via `AssignStreams`. Essa atribuição automática vem de um **contador global do processo** (`RngSeedManager`) que nunca volta a zero sozinho. O projeto já fixava explicitamente os streams da **mobilidade** (garantindo que a topologia/movimento fossem idênticos entre os 3 protocolos, para o mesmo `run`) — mas não fixava os streams do **Wi-Fi** (backoff do MAC) nem dos **protocolos de roteamento** (jitter de retransmissão de RREQ/HELLO/TC).

Como as 1440 execuções da matriz completa rodam no mesmo processo, e esse contador nunca é resetado entre elas, o resultado de uma execução passou a depender de **quantas execuções já tinham ocorrido antes dela no laço** — não só da semente e do número de `run`, quebrando a reprodutibilidade (e, pior, potencialmente enviesando a comparação entre protocolos, já que o laço processa `for protocolo { for velocidade { for run } } }`, então cada protocolo "herda" uma posição diferente nesse contador).

### A correção

Em `MontarCenario` (`src/manet-routing.cc`):

1. **Streams explícitos** para Wi-Fi (`WifiHelper::AssignStreams`) e para o protocolo de roteamento ativo (`AodvHelper`/`OlsrHelper::AssignStreams`; para o DSDV, que não expõe esse método no helper, a atribuição é feita nó a nó, diretamente na instância de `dsdv::RoutingProtocol`) — usando o mesmo contador local (`streamIndex`) já usado para a mobilidade, continuando a partir dela.
2. **`RngSeedManager::ResetNextStreamIndex()`** ao final de cada montagem de cenário, como rede de segurança — **testado e confirmado necessário**: streams explícitos sozinhos (item 1) não bastaram para eliminar a dependência de ordem (ainda sobra alguma variável aleatória interna do ns-3 fora do alcance de `AssignStreams`, não identificada precisamente). Só com os dois juntos o mesmo cenário dá sempre o mesmo resultado, em qualquer ordem de execução — pré-requisito, inclusive, para poder paralelizar a simulação (Seção 6).

### O efeito da correção nos dados

Comparando a rodada de 60 repetições **antes** e **depois** da correção: das 96 células (4 métricas × 12 combinações protocolo×velocidade × 2 estágios), nenhuma diferença sobrevive à correção de Holm — ou seja, **a correção não muda as conclusões gerais**, mas dá reprodutibilidade de verdade aos números, e limpa distorções pontuais (ex.: a cauda de atraso do AODV, antes com valores de até 400-640 ms em algumas execuções, ficou bem mais contida depois do fix — sinal de que parte dessa variação extrema vinha do bug, não só da física do cenário).

---

## 9. Principais resultados e observações

*(Dados de `data/processed/summary_20n.csv`, `summary_10n.csv`, `stats_tests_20n.csv`/`10n.csv` e `stats_nodes_comparison.csv`, pós-correção de RNG, 60 repetições.)*

### 9.1 Efeito do protocolo (mesma densidade)

A 20 nós, o teste de Mann-Whitney mostra:

- **PDR**: AODV e OLSR **empatam** em quase todas as velocidades (diferença não significativa, p ≥ 0,23), exceto a 20 m/s, onde o AODV se destaca (p = 0,003) — a vantagem clássica do reativo só aparece claramente em alta mobilidade. Ambos superam o DSDV em PDR, com significância em todas as velocidades (p < 0,001).
- **Atraso e jitter**: o AODV é significativamente maior que os outros dois em **todas** as velocidades (p < 0,001, delta de Cliff ≥ 0,68 — efeito grande). O mecanismo: fila de pacotes durante descoberta de rota (Seção 4.1) + viés de sobrevivência na métrica (Seção 5).

**Conclusão prática:** a suposição comum de que "reativo = sempre melhor PDR, sempre pior latência" só é parcialmente verdadeira aqui — é verdadeira contra o DSDV, mas o AODV não bate o OLSR em confiabilidade na maior parte das velocidades testadas.

### 9.2 Efeito da densidade de nós (20 vs 10, mesma área)

| Protocolo | PDR | Atraso | Jitter | Throughput |
|---|---|---|---|---|
| AODV | sem efeito (0/4 velocidades significativas) | pior com 20n em 2/4 | **pior com 20n em 4/4** | sem efeito |
| OLSR | sem efeito | pior com 20n em 1/4 (só 20 m/s) | pior com 20n em 1/4 (só 20 m/s) | sem efeito |
| DSDV | **melhor com 10n em 4/4** | sem efeito | sem efeito | **melhor com 10n em 4/4** |

O padrão é assimétrico e, no caso do DSDV, contraintuitivo: **menos nós significa melhor PDR** — o oposto do que normalmente se assume (menos nós = rede mais esparsa = pior conectividade). O mecanismo (Seção 4.3): o tamanho do broadcast periódico do DSDV cresce com o número total de nós, então mais nós significa mais colisão no meio compartilhado, prejudicando justamente o protocolo que deveria ganhar redundância de rotas com mais vizinhos. Já o AODV tem PDR estável nas duas densidades, mas paga em atraso/jitter quando há mais nós (mais saltos/retransmissores até o destino durante a descoberta de rota). O OLSR, graças ao MPR, é o mais resistente à densidade — só mostra efeito quando mobilidade alta se soma a mais nós.

**Isso generaliza para 200 vs 100 nós, mantendo a proporção de área?** Não necessariamente — ver discussão na Seção 10.

### 9.3 Nem toda variação visual é real

Com 60 repetições, muitas oscilações visuais entre velocidades adjacentes de um mesmo protocolo (ex.: um "sobe, desce, sobe" na curva de PDR ou delay) **não são estatisticamente significativas** — são ruído amostral, dentro da margem de erro (IC95%). Isso foi verificado diretamente com testes de hipótese par-a-par ao longo do desenvolvimento deste projeto: por exemplo, um salto aparentemente grande no gráfico de PDR do OLSR entre duas velocidades intermediárias não sobreviveu ao teste de Mann-Whitney, enquanto a queda de PDR do OLSR entre 10 e 20 m/s é real e consistente. A lição prática: **nunca interpretar um gráfico de linha ligando médias brutas sem olhar as barras de erro (ou, melhor, o teste de hipótese por trás) — padrões não-monotônicos em dados ruidosos são esperados, não um sinal de erro de simulação.**

### 9.4 PDR por fluxo é bimodal — a média esconde isso

Olhando a distribuição de PDR **por fluxo individual** (não a média da execução), o padrão não é "a maioria dos fluxos entrega ~75%" — é uma mistura de fluxos que funcionam quase perfeitamente e fluxos que quase não funcionam:

| Protocolo | Fluxos com PDR ≥ 95% | Fluxos com 0 recepção |
|---|---|---|
| AODV | 52,8% | 0,5% |
| OLSR | 35,0% | 8,3% |
| DSDV | 25,2% | 11,9% |

O que decide o resultado de um fluxo é principalmente se o par origem-destino está bem conectado na topologia sorteada daquela execução — não uma "taxa de perda típica". Isso também explica por que o AODV tem tão poucos fluxos com zero recepção (5 de 960, a 20 nós): ele persiste tentando descobrir rota, enquanto OLSR/DSDV simplesmente não conseguem convergir pra certos destinos na mesma topologia.

### 9.5 Throughput medido só no período ativo (artefato a observar)

Como a duração usada no cálculo de throughput é só o período entre o primeiro envio e o último recebimento *daquele fluxo específico* (não a janela de tráfego inteira), fluxos intermitentes (poucos pacotes, span curto) podem reportar throughput mais alto do que mostrariam se medidos na janela completa de 103 s. Vale ler o throughput como "vazão enquanto o fluxo esteve ativo", não como uma taxa sustentada. O PDR é a métrica mais fiel para julgar confiabilidade.

---

## 10. Limitações e perguntas em aberto

- **Escala (200 vs 100 nós, mantendo a densidade):** não dá para assumir que os efeitos de densidade observados aqui (Seção 9.2) se repetem proporcionalmente numa rede 10× maior. Manter a densidade (nós/m²) controla a conectividade *local*, mas não controla dois efeitos que crescem com o tamanho *global* da rede: (1) o tamanho do broadcast periódico do DSDV cresce com o número total de nós, não com a densidade local — o problema do DSDV tenderia a piorar, não ficar igual; (2) o diâmetro da rede em saltos cresce com a raiz quadrada da área (alcance de rádio é fixo), então uma rede 10× maior em nós e área teria rotas ~3× mais longas em saltos — o atraso/jitter do AODV tenderia a piorar também. O OLSR, por usar MPR (uma estrutura local), é o protocolo cujo mecanismo mais se aproxima de ser invariante a essa escala. Além disso, os parâmetros de tempo (HELLO, TC, ciclo do DSDV, warm-up) são constantes fixas, não ajustadas para o diâmetro maior da rede. Testar isso de verdade é um trabalho futuro natural.
- **Pareamento dos dados:** AODV, OLSR e DSDV compartilham a mesma seed de mobilidade para o mesmo `run` (por desenho, Seção 2) — ou seja, os dados são **pareados** por run. Os testes usados aqui (Mann-Whitney) tratam as amostras como independentes e perdem poder estatístico por ignorar esse pareamento; um teste de Wilcoxon pareado seria mais sensível (sugestão não aplicada neste trabalho).
- **Sem modelo de energia nem overhead de controle medido diretamente:** a explicação mecanística do efeito de densidade (Seção 9.2) é plausível e consistente com o funcionamento documentado de cada protocolo (Seção 4), mas não foi comprovada medindo bytes/pacotes de controle diretamente — ficaria mais forte com essa medição.
- **Cenário não é "IoT real":** tráfego CBR contínuo com 4 fluxos fixos, 802.11b a 11 Mbps — bem diferente de IoT real (tráfego esporádico orientado a eventos, payloads pequenos, PHY de baixa potência como 802.15.4/BLE/LoRa).
- **Uma única área e duas densidades:** generalização para outras áreas/densidades intermediárias não testada.

---

## 11. Mapa do repositório

```
manet-ns3/
├── README.md                     # documentação de decisões de projeto e como reproduzir
├── src/manet-routing.cc          # simulador ns-3 (cenário, protocolos, métricas, paralelização)
├── scripts/
│   ├── run_batch.sh              # compila, roda (paralelo), traz os dados de volta, chama analyze.py
│   └── analyze.py                # validação, agregação, estatística não paramétrica, gráficos
├── data/
│   ├── raw/                      # CSVs brutos por fluxo, log.txt, amostra de XML do FlowMonitor
│   └── processed/                # per_run_*.csv, summary_*.csv, descriptive_*.csv,
│                                  # stats_tests_*.csv, stats_nodes_comparison.csv,
│                                  # validation_report_*.txt
├── figures/                      # gráficos obrigatórios + boxplots + comparação 20 vs 10 nós
└── article/
    ├── artigo.tex / artigo.pdf   # artigo formal, formato SBC, 5 páginas
    └── documentacao-tecnica.md   # este documento
```

Para reproduzir tudo do zero, ver a Seção 9 ("Tutorial de reprodução completa") do `README.md`.
