/*
 * manet-routing.cc
 * ---------------------------------------------------------------------------
 * Trabalho Final — Redes Móveis (IFG, Câmpus Inhumas, 2026/2)
 * Comparação AODV x OLSR x DSDV em MANET, sob 4 níveis de mobilidade.
 *
 * COMO RODAR (sem nenhum argumento — tudo já está definido no código):
 *
 *     cp src/manet-routing.cc $NS3_DIR/scratch/manet-routing.cc
 *     cd $NS3_DIR
 *     ./ns3 build
 *     ./ns3 run scratch/manet-routing
 *
 * (o script scripts/run_batch.sh faz esses 4 passos automaticamente e ainda
 * copia o resultado de volta para data/raw/ deste projeto — ver README.md)
 *
 * O programa, sozinho:
 *   1) roda uma checagem rápida de cada protocolo (AODV, OLSR, DSDV) e
 *      imprime um checklist de validação (slide 16: "confirme se cada
 *      protocolo está realmente instalado");
 *   2) roda a matriz experimental completa — 3 protocolos x 4 velocidades
 *      x 5 repetições = 60 execuções (slide 21) — sem precisar editar nada
 *      nem passar parâmetro nenhum.
 *
 * Saída: resultado_simulacao/resultados.csv, UMA LINHA POR FLUXO (4 por
 * execução, 240 linhas no total), exatamente nas colunas recomendadas pelo
 * slide 24:
 *   protocol,speed,run,flowId,txPackets,rxPackets,lostPackets,rxBytes,
 *   throughputMbps,pdrPct,delayMs,jitterMs
 *
 * ---------------------------------------------------------------------------
 * O QUE O ENUNCIADO (slides) DEFINE, E ONDE:
 *   20 nós, área 500x500 m, 802.11 Ad Hoc, RandomWaypointMobilityModel,
 *   UDP, 4 fluxos simultâneos, 120 s de simulação, velocidades 1/5/10/20 m/s,
 *   AODV/OLSR/DSDV, mínimo 5 repetições, seed mestre fixa + run variável
 *   (slide 9 e 22). Isso está todo fixado nas constantes abaixo.
 *
 * O QUE O ENUNCIADO NÃO ESPECIFICA (decisão do grupo, documentada aqui):
 *   - Taxa PHY 802.11b: DsssRate11Mbps, para dados, controle E broadcast
 *     (NonUnicastMode). Sem igualar o broadcast à taxa dos dados, o HELLO/TC/
 *     updates dos protocolos viajam numa taxa mais baixa (logo com alcance
 *     maior) que os pacotes de dados — o protocolo "acha" que tem rota para
 *     um vizinho que na prática não recebe o dado. É o próprio exemplo
 *     oficial do ns-3 (examples/routing/manet-routing-compare.cc) que faz
 *     essa igualação.
 *   - Propagação: FriisPropagationLossModel em 2,412 GHz. O valor padrão do
 *     Friis no ns-3 assume 5,15 GHz, o que é incoerente com 802.11b (2,4 GHz)
 *     e reduz artificialmente o alcance do rádio.
 *   - Potência de Tx: 7,5 dBm (mesmo valor do exemplo oficial do ns-3).
 *   - Velocidade no RandomWaypoint: TODOS os nós se movem à mesma velocidade
 *     constante --speed-- (ConstantRandomVariable), igual para todas as
 *     execuções da mesma linha da matriz.
 *   - Pause time: 1,0 s, constante em todas as execuções.
 *   - Distribuição inicial: uniforme na área (RandomRectanglePositionAllocator).
 *   - Streams aleatórios da mobilidade fixados (AssignStreams): para o mesmo
 *     "run", a topologia inicial e o movimento são IDÊNTICOS nos 3
 *     protocolos — a diferença nos resultados vem só do protocolo, não de
 *     "sorte" de topologia.
 *   - Fluxos UDP: fluxo i = nó i -> nó (19-i), porta 9+i, pacote de 1024
 *     bytes a cada 0,1 s (mesmos valores do código original do grupo).
 *   - Início do tráfego (warm-up): 15 s. É o tempo de um ciclo completo de
 *     atualização periódica do DSDV (15 s, ver Model Library) e várias
 *     rodadas de HELLO/TC do OLSR (2 s / 5 s); antes disso as tabelas de
 *     roteamento ainda não convergiram. O tempo TOTAL de simulação continua
 *     os 120 s exigidos pelo enunciado — o warm-up só define QUANDO, dentro
 *     desses 120 s, o tráfego de dados começa.
 *   - Fim do tráfego: 2 s antes do fim (118 s), para dar tempo de pacotes já
 *     enviados chegarem antes da simulação acabar.
 *   - Duração útil do throughput (slide 20 pede para documentar a escolha):
 *     timeLastRxPacket - timeFirstTxPacket, DO PRÓPRIO FLUXO. Se o fluxo não
 *     recebeu nenhum pacote, throughput = 0.
 * ---------------------------------------------------------------------------
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/aodv-module.h"
#include "ns3/olsr-module.h"
#include "ns3/dsdv-module.h"
#include "ns3/flow-monitor-module.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("ManetRoutingCompare");

// =============================================================================
// 1. TUDO que o enunciado fixa (slide 9) + as decisões do grupo (ver acima).
//    Nada disso é lido de linha de comando: é exatamente o que vai rodar.
// =============================================================================
static const uint32_t    NUM_NODES   = 20;      // slide 9
static const double      AREA_M      = 500.0;   // slide 9 (área 500 x 500 m)
static const double      SIM_TIME_S  = 120.0;   // slide 9 (tempo de simulação)
static const uint32_t    NUM_FLOWS   = 4;        // slide 9 (fluxos simultâneos)
static const uint32_t    MASTER_SEED = 12345;    // slide 22 (seed mestre fixa)

static const std::vector<std::string> PROTOCOLS = {"aodv", "olsr", "dsdv"};      // slide 9/21
static const std::vector<double>      SPEEDS    = {1.0, 5.0, 10.0, 20.0};        // slide 9/21
static const uint32_t                 NUM_RUNS  = 5;                             // slide 9/21 (mínimo 5)

// ---- Decisões do grupo (não exigidas pelo enunciado; ver comentário no topo) ----
static const double      PAUSE_S       = 1.0;
static const double      WARMUP_S      = 15.0;               // início do tráfego
static const double      GUARD_S       = 2.0;                // tráfego termina em SIM_TIME_S - GUARD_S
static const double      PKT_INTERVAL  = 0.1;                 // s entre pacotes (igual ao código original)
static const uint32_t    PKT_SIZE      = 1024;                // bytes (igual ao código original)
static const uint16_t    BASE_PORT     = 9;
static const double      TX_POWER_DBM  = 7.5;                 // exemplo oficial ns-3
static const double      CARRIER_HZ    = 2.412e9;              // canal 1, 2,4 GHz (802.11b)
static const std::string PHY_MODE      = "DsssRate11Mbps";

static const std::string OUT_DIR  = "resultado_simulacao";
static const std::string OUT_CSV  = OUT_DIR + "/resultados.csv";

// =============================================================================
// 2. Utilidades
// =============================================================================

// Nome do protocolo de roteamento efetivamente instalado no nó (para a
// validação do slide 16: "confirme se cada protocolo está realmente
// instalado").
static std::string
RoutingProtocolName (Ptr<Node> node)
{
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  if (!ipv4 || !ipv4->GetRoutingProtocol ())
    {
      return "(nenhum)";
    }
  Ptr<Ipv4ListRouting> list = DynamicCast<Ipv4ListRouting> (ipv4->GetRoutingProtocol ());
  if (!list)
    {
      return ipv4->GetRoutingProtocol ()->GetInstanceTypeId ().GetName ();
    }
  int16_t priority = 0;
  return list->GetRoutingProtocol (0, priority)->GetInstanceTypeId ().GetName ();
}

static std::string
ExpectedTypeId (const std::string &protocol)
{
  if (protocol == "aodv") return "ns3::aodv::RoutingProtocol";
  if (protocol == "olsr") return "ns3::olsr::RoutingProtocol";
  return "ns3::dsdv::RoutingProtocol";
}

// =============================================================================
// 3. Monta o cenário-base (comum à validação e às execuções da matriz).
//    Devolve os nós, os dispositivos e as interfaces já com IP, com o
//    protocolo de roteamento (aodv/olsr/dsdv) e a mobilidade instalados.
// =============================================================================
struct Cenario
{
  NodeContainer nodes;
  NetDeviceContainer devices;
  Ipv4InterfaceContainer interfaces;
};

static Cenario
MontarCenario (const std::string &protocol, double speed, uint32_t run)
{
  // ---- Aleatoriedade: seed mestre fixa + run variável (slide 22) ----
  RngSeedManager::SetSeed (MASTER_SEED);
  RngSeedManager::SetRun (run);

  // Broadcast (HELLO/TC/updates) na MESMA taxa dos dados — ver decisão no topo.
  Config::SetDefault ("ns3::WifiRemoteStationManager::NonUnicastMode", StringValue (PHY_MODE));

  Cenario c;
  c.nodes.Create (NUM_NODES);

  // ---- Wi-Fi 802.11b Ad Hoc (slide 6) ----
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211b);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                "DataMode", StringValue (PHY_MODE),
                                "ControlMode", StringValue (PHY_MODE));

  YansWifiPhyHelper wifiPhy;
  YansWifiChannelHelper wifiChannel;
  wifiChannel.SetPropagationDelay ("ns3::ConstantSpeedPropagationDelayModel");
  wifiChannel.AddPropagationLoss ("ns3::FriisPropagationLossModel",
                                  "Frequency", DoubleValue (CARRIER_HZ));
  wifiPhy.SetChannel (wifiChannel.Create ());
  wifiPhy.Set ("TxPowerStart", DoubleValue (TX_POWER_DBM));
  wifiPhy.Set ("TxPowerEnd", DoubleValue (TX_POWER_DBM));

  WifiMacHelper wifiMac;
  wifiMac.SetType ("ns3::AdhocWifiMac"); // Ad Hoc, sem AP (slide 6)
  c.devices = wifi.Install (wifiPhy, wifiMac, c.nodes);

  // ---- Roteamento: escolhe AODV, OLSR ou DSDV (slide 16) ----
  AodvHelper aodv;
  OlsrHelper olsr;
  DsdvHelper dsdv;
  Ipv4ListRoutingHelper listRouting;
  if (protocol == "aodv")
    {
      listRouting.Add (aodv, 100);
    }
  else if (protocol == "olsr")
    {
      listRouting.Add (olsr, 100);
    }
  else if (protocol == "dsdv")
    {
      listRouting.Add (dsdv, 100);
    }
  else
    {
      NS_FATAL_ERROR ("Protocolo desconhecido: " << protocol);
    }

  InternetStackHelper internet;
  internet.SetRoutingHelper (listRouting);
  internet.Install (c.nodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.1.0", "255.255.255.0");
  c.interfaces = ipv4.Assign (c.devices);

  // ---- Mobilidade: RandomWaypoint, velocidade constante, streams fixos ----
  int64_t streamIndex = 0;

  Ptr<RandomRectanglePositionAllocator> posAlloc = CreateObject<RandomRectanglePositionAllocator> ();
  Ptr<UniformRandomVariable> xVar = CreateObject<UniformRandomVariable> ();
  xVar->SetAttribute ("Min", DoubleValue (0.0));
  xVar->SetAttribute ("Max", DoubleValue (AREA_M));
  Ptr<UniformRandomVariable> yVar = CreateObject<UniformRandomVariable> ();
  yVar->SetAttribute ("Min", DoubleValue (0.0));
  yVar->SetAttribute ("Max", DoubleValue (AREA_M));
  posAlloc->SetX (xVar);
  posAlloc->SetY (yVar);
  streamIndex += posAlloc->AssignStreams (streamIndex);

  std::ostringstream speedRv, pauseRv;
  speedRv << "ns3::ConstantRandomVariable[Constant=" << speed << "]";
  pauseRv << "ns3::ConstantRandomVariable[Constant=" << PAUSE_S << "]";

  MobilityHelper mobility;
  mobility.SetPositionAllocator (posAlloc);
  mobility.SetMobilityModel ("ns3::RandomWaypointMobilityModel",
                             "Speed", StringValue (speedRv.str ()),
                             "Pause", StringValue (pauseRv.str ()),
                             "PositionAllocator", PointerValue (posAlloc));
  mobility.Install (c.nodes);
  streamIndex += mobility.AssignStreams (c.nodes, streamIndex);

  return c;
}

// =============================================================================
// 4. Validação rápida (slide 16): roda um cenário pequeno para cada
//    protocolo e confirma nós, mobilidade, Wi-Fi Ad Hoc e protocolo
//    realmente instalados. NÃO grava no CSV de resultados — é só um
//    checklist impresso no console, chamado automaticamente antes do lote.
// =============================================================================
static bool
ValidarProtocolo (const std::string &protocol)
{
  std::cout << "\n---- Validando " << protocol << " ----" << std::endl;
  bool ok = true;

  // Cenário pequeno e rápido, só para checagem (não usa as constantes do
  // experimento-base; run=9999 para nunca colidir com os runs 1..5 reais).
  Cenario c = MontarCenario (protocol, /*speed*/ 5.0, /*run*/ 9999);

  auto Check = [&ok] (bool cond, const std::string &msg) {
    std::cout << (cond ? "  [OK]    " : "  [FALHA] ") << msg << std::endl;
    if (!cond) ok = false;
  };

  Check (c.nodes.GetN () == NUM_NODES,
         "nos criados: " + std::to_string (c.nodes.GetN ()));

  bool allAdhoc = true;
  for (uint32_t i = 0; i < c.devices.GetN (); ++i)
    {
      Ptr<WifiNetDevice> wd = DynamicCast<WifiNetDevice> (c.devices.Get (i));
      allAdhoc = allAdhoc && wd && wd->GetMac ()->GetInstanceTypeId ().GetName () == "ns3::AdhocWifiMac";
    }
  Check (allAdhoc, "Wi-Fi 802.11b em modo Ad Hoc (AdhocWifiMac, sem AP) em todos os nos");

  bool allRwp = true;
  for (uint32_t i = 0; i < c.nodes.GetN (); ++i)
    {
      Ptr<MobilityModel> mob = c.nodes.Get (i)->GetObject<MobilityModel> ();
      allRwp = allRwp && mob && mob->GetInstanceTypeId ().GetName () == "ns3::RandomWaypointMobilityModel";
    }
  Check (allRwp, "mobilidade RandomWaypointMobilityModel instalada em todos os nos");

  std::string esperado = ExpectedTypeId (protocol);
  bool allProto = true;
  for (uint32_t i = 0; i < c.nodes.GetN (); ++i)
    {
      allProto = allProto && (RoutingProtocolName (c.nodes.Get (i)) == esperado);
    }
  Check (allProto, "protocolo instalado em todos os nos: " + RoutingProtocolName (c.nodes.Get (0)) +
                       " (esperado " + esperado + ")");

  Simulator::Destroy ();
  std::cout << "---- " << protocol << ": " << (ok ? "VALIDACAO OK" : "VALIDACAO FALHOU") << " ----"
            << std::endl;
  return ok;
}

// =============================================================================
// 5. Uma execução completa da matriz (protocolo x velocidade x run):
//    monta o cenário, cria os 4 fluxos UDP, roda o FlowMonitor e grava
//    1 linha por fluxo no CSV, exatamente nas colunas do slide 24.
// =============================================================================
static void
RunExperiment (const std::string &protocol, double speed, uint32_t run)
{
  Cenario c = MontarCenario (protocol, speed, run);

  const double trafficStop = SIM_TIME_S - GUARD_S; // 118 s

  // ---- Tráfego UDP: 4 fluxos simultâneos e determinísticos ----
  // fluxo i: no i -> no (NUM_NODES-1-i), porta BASE_PORT+i
  for (uint32_t i = 0; i < NUM_FLOWS; ++i)
    {
      uint32_t senderId = i;
      uint32_t receiverId = (NUM_NODES - 1) - i;
      uint16_t port = BASE_PORT + i;

      UdpServerHelper server (port);
      ApplicationContainer serverApp = server.Install (c.nodes.Get (receiverId));
      serverApp.Start (Seconds (1.0));
      serverApp.Stop (Seconds (SIM_TIME_S));

      UdpClientHelper client (c.interfaces.GetAddress (receiverId), port);
      client.SetAttribute ("MaxPackets", UintegerValue (100000));
      client.SetAttribute ("Interval", TimeValue (Seconds (PKT_INTERVAL)));
      client.SetAttribute ("PacketSize", UintegerValue (PKT_SIZE));

      ApplicationContainer clientApp = client.Install (c.nodes.Get (senderId));
      clientApp.Start (Seconds (WARMUP_S));   // tráfego começa após o aquecimento
      clientApp.Stop (Seconds (trafficStop)); // termina com folga antes do fim da simulacao
    }

  // ---- FlowMonitor (slide 7/19) ----
  FlowMonitorHelper flowmon;
  Ptr<FlowMonitor> monitor = flowmon.InstallAll ();

  Simulator::Stop (Seconds (SIM_TIME_S)); // tempo TOTAL de simulacao = 120 s (slide 9)
  Simulator::Run ();

  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

  std::ofstream csv (OUT_CSV, std::ios::app);
  csv << std::fixed << std::setprecision (6);

  uint32_t somaTx = 0, somaRx = 0;

  // Uma linha por fluxo de DADOS (identificado pela origem/destino/porta;
  // ignora o trafego de controle dos proprios protocolos de roteamento).
  for (uint32_t i = 0; i < NUM_FLOWS; ++i)
    {
      Ipv4Address src = c.interfaces.GetAddress (i);
      Ipv4Address dst = c.interfaces.GetAddress ((NUM_NODES - 1) - i);
      uint16_t port = BASE_PORT + i;

      FlowId flowId = 0;
      uint32_t tx = 0, rx = 0, lost = 0;
      uint64_t rxBytes = 0;
      Time delaySum = Seconds (0), jitterSum = Seconds (0);
      Time tFirstTx = Seconds (0), tLastRx = Seconds (0);

      for (auto &kv : stats)
        {
          Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (kv.first);
          if (t.protocol == 17 && t.sourceAddress == src && t.destinationAddress == dst &&
              t.destinationPort == port)
            {
              flowId = kv.first;
              tx = kv.second.txPackets;
              rx = kv.second.rxPackets;
              lost = kv.second.lostPackets;
              rxBytes = kv.second.rxBytes;
              delaySum = kv.second.delaySum;
              jitterSum = kv.second.jitterSum;
              tFirstTx = kv.second.timeFirstTxPacket;
              tLastRx = kv.second.timeLastRxPacket;
              break;
            }
        }

      // ---- Metricas obrigatorias (formulas do slide 20) ----
      double pdrPct = (tx > 0) ? (100.0 * rx / tx) : 0.0;
      double delayMs = (rx > 0) ? (1000.0 * delaySum.GetSeconds () / rx) : 0.0;
      double jitterMs = (rx > 1) ? (1000.0 * jitterSum.GetSeconds () / (rx - 1)) : 0.0;

      // Duracao util = timeLastRxPacket - timeFirstTxPacket DO FLUXO (decisao
      // documentada no topo do arquivo).
      double duration = (rx > 0 && tLastRx > tFirstTx) ? (tLastRx - tFirstTx).GetSeconds () : 0.0;
      double throughputMbps = (duration > 0) ? (rxBytes * 8.0) / duration / 1e6 : 0.0;

      // Colunas exatamente na ordem recomendada pelo slide 24:
      csv << protocol << "," << speed << "," << run << "," << flowId << "," << tx << "," << rx
          << "," << lost << "," << rxBytes << "," << throughputMbps << "," << pdrPct << ","
          << delayMs << "," << jitterMs << "\n";

      somaTx += tx;
      somaRx += rx;
    }
  csv.close ();

  double pdrGeral = (somaTx > 0) ? (100.0 * somaRx / somaTx) : 0.0;
  std::cout << "  " << protocol << " | " << speed << " m/s | run " << run << " -> tx=" << somaTx
            << " rx=" << somaRx << " PDR=" << pdrGeral << "%" << std::endl;

  Simulator::Destroy ();
}

// =============================================================================
// 6. main(): roda TUDO sozinho, sem nenhum argumento.
// =============================================================================
int
main (int argc, char *argv[])
{
  CommandLine cmd (__FILE__); // sem opcoes: nada e' passado pelo usuario
  cmd.Parse (argc, argv);

  std::filesystem::create_directories (OUT_DIR);

  // ---- 1) Validacao rapida de cada protocolo (slide 16) ----
  std::cout << "===================================================" << std::endl;
  std::cout << " ETAPA 1/2: validando AODV, OLSR e DSDV" << std::endl;
  std::cout << "===================================================" << std::endl;
  bool todasOk = true;
  for (const std::string &p : PROTOCOLS)
    {
      todasOk &= ValidarProtocolo (p);
    }
  if (!todasOk)
    {
      std::cout << "\nATENCAO: pelo menos um protocolo falhou na validacao (ver acima)."
                << " Prosseguindo mesmo assim para o lote completo." << std::endl;
    }
  else
    {
      std::cout << "\nTodos os protocolos validados com sucesso." << std::endl;
    }

  // ---- 2) Matriz experimental completa: 3 x 4 x 5 = 60 execucoes ----
  std::cout << "\n===================================================" << std::endl;
  std::cout << " ETAPA 2/2: matriz experimental (" << PROTOCOLS.size () << " protocolos x "
            << SPEEDS.size () << " velocidades x " << NUM_RUNS << " runs = "
            << PROTOCOLS.size () * SPEEDS.size () * NUM_RUNS << " execucoes)" << std::endl;
  std::cout << "===================================================" << std::endl;

  // CSV unico com todos os resultados; cabecalho escrito uma vez (trunca
  // qualquer resultado de uma execucao anterior).
  {
    std::ofstream csv (OUT_CSV, std::ios::trunc);
    csv << "protocol,speed,run,flowId,txPackets,rxPackets,lostPackets,rxBytes,"
           "throughputMbps,pdrPct,delayMs,jitterMs\n";
  }

  uint32_t execucao = 0;
  const uint32_t total = PROTOCOLS.size () * SPEEDS.size () * NUM_RUNS;
  for (const std::string &p : PROTOCOLS)
    {
      for (double s : SPEEDS)
        {
          for (uint32_t r = 1; r <= NUM_RUNS; ++r)
            {
              ++execucao;
              std::cout << "[" << execucao << "/" << total << "] " << p << ", " << s
                        << " m/s, run " << r << "..." << std::endl;
              RunExperiment (p, s, r);
            }
        }
    }

  std::cout << "\nConcluido. Resultados em " << OUT_CSV << " (" << total * NUM_FLOWS
            << " linhas esperadas: " << total << " execucoes x " << NUM_FLOWS << " fluxos)."
            << std::endl;
  return 0;
}
