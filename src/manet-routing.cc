/*
 * manet-routing.cc
 * ---------------------------------------------------------------------------
 * Comparação AODV x OLSR x DSDV em uma MANET (802.11b ad hoc, UDP), sob
 * diferentes velocidades de mobilidade.
 *
 * COMO RODAR (sem nenhum argumento — tudo já está definido no código):
 *
 *     cp src/manet-routing.cc $NS3_DIR/scratch/manet-routing.cc
 *     cd $NS3_DIR
 *     ./ns3 build
 *     ./ns3 run scratch/manet-routing
 *
 * O programa, sozinho:
 *   1) roda uma checagem rápida de cada protocolo (AODV, OLSR, DSDV),
 *      confirmando que cada um foi realmente instalado nos nós antes de
 *      confiar em qualquer resultado;
 *   2) roda a matriz experimental completa: 3 protocolos x 4 velocidades
 *      x 5 repetições = 60 execuções, sem precisar editar nada nem passar
 *      parâmetro nenhum.
 *
 * Saída: resultado_simulacao/resultados.csv, uma linha por fluxo UDP (4 por
 * execução, 240 linhas no total):
 *   protocol,speed,run,flowId,txPackets,rxPackets,lostPackets,rxBytes,
 *   throughputMbps,pdrPct,delayMs,jitterMs
 *
 * ---------------------------------------------------------------------------
 * CENÁRIO FIXO (ver constantes logo abaixo):
 *   20 nós, área 500x500 m, Wi-Fi 802.11b em modo ad hoc (sem AP),
 *   RandomWaypointMobilityModel, tráfego UDP, 4 fluxos simultâneos,
 *   120 s de simulação, velocidades 1/5/10/20 m/s, protocolos AODV/OLSR/DSDV,
 *   5 repetições por combinação, seed mestre fixa com run variável por
 *   repetição.
 *
 * NOTAS DE IMPLEMENTAÇÃO (detalhes que não têm uma única resposta óbvia,
 * então ficam documentados aqui para quem for ler o código depois):
 *
 *   - Taxa PHY 802.11b (DsssRate11Mbps) é usada para dados, controle E
 *     broadcast (NonUnicastMode). Se o broadcast ficasse na taxa básica
 *     padrão — mais baixa, logo com alcance maior que a dos dados — HELLO,
 *     TC e updates de roteamento alcançariam vizinhos que, na prática, não
 *     conseguem receber o tráfego de dados na taxa configurada. O resultado
 *     seria rota "visível" na tabela que não entrega pacote de verdade.
 *
 *   - A propagação usa FriisPropagationLossModel configurado para 2,412 GHz
 *     (canal 1 do 802.11b/2,4 GHz) em vez do padrão do ns-3 para esse
 *     modelo, que assume 5,15 GHz. Sem essa correção, o alcance do rádio sai
 *     menor do que o fisicamente esperado para 802.11b.
 *
 *   - Potência de transmissão fixa em 7,5 dBm, igual em todas as execuções.
 *
 *   - No RandomWaypoint, todos os nós se movem à mesma velocidade constante
 *     (um valor por rodada da matriz, não uma faixa aleatória), com pause
 *     time de 1,0 s e distribuição inicial uniforme na área.
 *
 *   - Os streams de números aleatórios da mobilidade são fixados
 *     explicitamente (AssignStreams). Isso garante que, para o mesmo número
 *     de run, a topologia inicial e o movimento dos nós sejam idênticos nos
 *     três protocolos — qualquer diferença nos resultados vem do protocolo
 *     em si, não de uma topologia sorteada diferente por acaso.
 *
 *   - Os 4 fluxos UDP são fixos e determinísticos: fluxo i liga o nó i ao
 *     nó (19-i), na porta 9+i, com pacotes de 1024 bytes a cada 0,1 s.
 *
 *   - O tráfego de dados só começa em 15 s de simulação (não em t=0), para
 *     dar tempo dos protocolos convergirem antes de medir qualquer coisa:
 *     o DSDV tem um ciclo de atualização periódica de 15 s, e o OLSR usa
 *     HELLO a cada 2 s e TC a cada 5 s — antes disso as tabelas de
 *     roteamento ainda não estabilizaram. O tempo total de simulação
 *     continua sendo 120 s; esse atraso só define QUANDO, dentro desses
 *     120 s, o tráfego de dados começa.
 *
 *   - O tráfego para 2 s antes do fim da simulação (em 118 s), dando tempo
 *     de pacotes já enviados chegarem antes do FlowMonitor parar de contar.
 *
 *   - Duração útil usada no cálculo de throughput: timeLastRxPacket menos
 *     timeFirstTxPacket, calculada por fluxo. Se o fluxo não recebeu nenhum
 *     pacote, o throughput desse fluxo é 0.
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
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("ManetRoutingCompare");

// =============================================================================
//                              CONSTANTES
// =============================================================================

// ---- Cenário experimental ----
static const uint32_t NUM_NODES   = 20;    // número de nós da rede
static const double   AREA_M      = 500.0; // lado da área quadrada, em metros
static const double   SIM_TIME_S  = 120.0; // tempo total de simulação, em segundos
static const uint32_t NUM_FLOWS   = 4;     // fluxos UDP simultâneos
static const uint32_t MASTER_SEED = 12345; // seed usada em todas as execuções

static const std::vector<std::string> PROTOCOLS = {"aodv", "olsr", "dsdv"}; // protocolos comparados
static const std::vector<double>      SPEEDS    = {1.0, 5.0, 10.0, 20.0};   // velocidades testadas (m/s)
static const uint32_t                 NUM_RUNS  = 5; // repetições independentes por combinação

// ---- Parâmetros auxiliares do cenário (ver "Notas de implementação" acima) ----
static const double      PAUSE_S       = 1.0;    // tempo parado em cada waypoint do RandomWaypoint
static const double      WARMUP_S      = 15.0;   // instante em que o tráfego de dados começa
static const double      GUARD_S       = 2.0;    // tráfego termina em SIM_TIME_S - GUARD_S
static const double      PKT_INTERVAL  = 0.1;    // intervalo entre pacotes, em segundos
static const uint32_t    PKT_SIZE      = 1024;   // tamanho do pacote UDP, em bytes
static const uint16_t    BASE_PORT     = 9;      // porta do fluxo 0; fluxo i usa a porta BASE_PORT+i
static const double      TX_POWER_DBM  = 7.5;    // potência de transmissão, em dBm
static const double      CARRIER_HZ    = 2.412e9; // frequência de portadora (canal 1, 2,4 GHz)
static const std::string PHY_MODE      = "DsssRate11Mbps"; // taxa 802.11b usada por dados e controle

static const std::string OUT_DIR  = "resultado_simulacao";
static const std::string OUT_CSV  = OUT_DIR + "/resultados.csv";

// =============================================================================
// Funções auxiliares
// =============================================================================

// Retorna o nome (TypeId) do protocolo de roteamento efetivamente instalado
// no nó. Usado só para confirmar, antes de rodar qualquer experimento, que o
// protocolo pedido foi mesmo o que acabou instalado.
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

// Nome do TypeId que cada string de protocolo deveria produzir uma vez
// instalada (usado para comparar com o retorno de RoutingProtocolName).
static std::string
ExpectedTypeId (const std::string &protocol)
{
  if (protocol == "aodv") return "ns3::aodv::RoutingProtocol";
  if (protocol == "olsr") return "ns3::olsr::RoutingProtocol";
  return "ns3::dsdv::RoutingProtocol";
}

// =============================================================================
// Monta o cenário de rede (Wi-Fi + roteamento + mobilidade), compartilhado
// pela checagem de validação e pelas execuções da matriz experimental.
// Devolve os nós, os dispositivos de rede e as interfaces já com IP.
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
  // Seed fixa + número de run: isso é o que torna cada repetição
  // independente (uma sequência de números aleatórios diferente por run) e,
  // ao mesmo tempo, inteiramente reproduzível (repetir o mesmo run dá
  // sempre o mesmo resultado).
  RngSeedManager::SetSeed (MASTER_SEED);
  RngSeedManager::SetRun (run);

  // Iguala a taxa do tráfego broadcast (HELLO/TC/updates dos protocolos de
  // roteamento) à taxa dos dados — ver "Notas de implementação" no topo.
  Config::SetDefault ("ns3::WifiRemoteStationManager::NonUnicastMode", StringValue (PHY_MODE));

  Cenario c;
  c.nodes.Create (NUM_NODES);

  // ---- Wi-Fi 802.11b em modo ad hoc (sem access point) ----
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211b);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                "DataMode", StringValue (PHY_MODE),
                                "ControlMode", StringValue (PHY_MODE));

  YansWifiPhyHelper wifiPhy;
  YansWifiChannelHelper wifiChannel;
  wifiChannel.SetPropagationDelay ("ns3::ConstantSpeedPropagationDelayModel");
  // Frequência de portadora corrigida para 2,4 GHz (ver nota no topo sobre
  // o padrão do FriisPropagationLossModel assumir 5,15 GHz).
  wifiChannel.AddPropagationLoss ("ns3::FriisPropagationLossModel",
                                  "Frequency", DoubleValue (CARRIER_HZ));
  wifiPhy.SetChannel (wifiChannel.Create ());
  wifiPhy.Set ("TxPowerStart", DoubleValue (TX_POWER_DBM));
  wifiPhy.Set ("TxPowerEnd", DoubleValue (TX_POWER_DBM));

  WifiMacHelper wifiMac;
  wifiMac.SetType ("ns3::AdhocWifiMac");
  c.devices = wifi.Install (wifiPhy, wifiMac, c.nodes);

  // ---- Protocolo de roteamento: AODV, OLSR ou DSDV ----
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
  // Fixar os streams aqui garante que, para o mesmo "run", a topologia
  // inicial e o movimento sejam idênticos nos três protocolos — qualquer
  // diferença de resultado vem do protocolo, não de uma topologia diferente
  // por acaso.
  streamIndex += mobility.AssignStreams (c.nodes, streamIndex);

  return c;
}

// =============================================================================
// Validação rápida: monta um cenário pequeno para um protocolo e confirma
// que os nós foram criados, a mobilidade e o Wi-Fi ad hoc estão configurados
// e o protocolo de roteamento realmente instalado é o esperado. Não grava
// nada no CSV de resultados — é só um checklist impresso no console, usado
// como sanidade antes de confiar na matriz experimental completa.
// =============================================================================
static bool
ValidarProtocolo (const std::string &protocol)
{
  std::cout << "\n---- Validando " << protocol << " ----" << std::endl;
  bool ok = true;

  // Cenário pequeno, só para checagem — run=9999 para nunca colidir com os
  // runs 1..5 usados nos experimentos de verdade.
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
  Check (allAdhoc, "Wi-Fi 802.11b em modo ad hoc (AdhocWifiMac, sem AP) em todos os nos");

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
// Roda uma execução completa (um protocolo, uma velocidade, um run): monta
// o cenário, cria os 4 fluxos UDP, roda o FlowMonitor e grava uma linha por
// fluxo no CSV de resultados.
// =============================================================================
static void
RunExperiment (const std::string &protocol, double speed, uint32_t run)
{
  Cenario c = MontarCenario (protocol, speed, run);

  const double trafficStop = SIM_TIME_S - GUARD_S; // tráfego termina em 118 s

  // ---- Tráfego UDP: 4 fluxos simultâneos e determinísticos ----
  // fluxo i liga o nó i ao nó (NUM_NODES-1-i), na porta BASE_PORT+i.
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
      clientApp.Start (Seconds (WARMUP_S));   // tráfego começa só depois do tempo de convergência
      clientApp.Stop (Seconds (trafficStop)); // termina com folga antes do fim da simulação
    }

  // ---- Medição de tráfego com o FlowMonitor ----
  FlowMonitorHelper flowmon;
  Ptr<FlowMonitor> monitor = flowmon.InstallAll ();

  Simulator::Stop (Seconds (SIM_TIME_S));
  Simulator::Run ();

  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

  std::ofstream csv (OUT_CSV, std::ios::app);
  csv << std::fixed << std::setprecision (6);

  uint32_t somaTx = 0, somaRx = 0;

  // Uma linha por fluxo de dados, identificado pela combinação de origem,
  // destino e porta — isso separa os 4 fluxos de tráfego do tráfego de
  // controle gerado pelo próprio protocolo de roteamento, que também
  // aparece nas estatísticas do FlowMonitor mas não deve entrar no CSV.
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

      // PDR: percentual de pacotes transmitidos que chegaram ao destino.
      double pdrPct = (tx > 0) ? (100.0 * rx / tx) : 0.0;
      // Atraso médio fim a fim, em milissegundos.
      double delayMs = (rx > 0) ? (1000.0 * delaySum.GetSeconds () / rx) : 0.0;
      // Jitter médio (variação do atraso entre pacotes consecutivos), em ms.
      double jitterMs = (rx > 1) ? (1000.0 * jitterSum.GetSeconds () / (rx - 1)) : 0.0;

      // Duração útil = intervalo entre o primeiro pacote enviado e o
      // último recebido, nesse fluxo. Se não houve recepção, throughput = 0.
      double duration = (rx > 0 && tLastRx > tFirstTx) ? (tLastRx - tFirstTx).GetSeconds () : 0.0;
      double throughputMbps = (duration > 0) ? (rxBytes * 8.0) / duration / 1e6 : 0.0;

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
// main(): roda tudo sozinho, sem nenhum argumento de linha de comando.
// =============================================================================
int
main (int argc, char *argv[])
{
  CommandLine cmd (__FILE__); // nenhuma opção registrada: nada é lido do usuário
  cmd.Parse (argc, argv);

  std::filesystem::create_directories (OUT_DIR);

  // ---- Etapa 1: valida rapidamente cada protocolo antes de confiar nele ----
  std::cout << "===================================================" << std::endl;
  std::cout << " ETAPA 1/2: validando AODV, OLSR e DSDV" << std::endl;
  std::cout << "===================================================" << std::endl;
  bool todasOk = true;
  for (const std::string &p : PROTOCOLS)
    {
      todasOk = todasOk && ValidarProtocolo (p);
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

  // ---- Etapa 2: matriz experimental completa (protocolos x velocidades x runs) ----
  std::cout << "\n===================================================" << std::endl;
  std::cout << " ETAPA 2/2: matriz experimental (" << PROTOCOLS.size () << " protocolos x "
            << SPEEDS.size () << " velocidades x " << NUM_RUNS << " runs = "
            << PROTOCOLS.size () * SPEEDS.size () * NUM_RUNS << " execucoes)" << std::endl;
  std::cout << "===================================================" << std::endl;

  // CSV único com todos os resultados; cabeçalho escrito uma vez, truncando
  // qualquer resultado de uma execução anterior do programa.
  {
    std::ofstream csv (OUT_CSV, std::ios::trunc);
    csv << "protocol,speed,run,flowId,txPackets,rxPackets,lostPackets,rxBytes,"
           "throughputMbps,pdrPct,delayMs,jitterMs\n";
  }

  uint32_t execucao = 0;
  const uint32_t total =
      static_cast<uint32_t> (PROTOCOLS.size () * SPEEDS.size () * NUM_RUNS);
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
