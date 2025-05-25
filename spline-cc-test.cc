#include "ns3/traffic-control-helper.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include <iostream>
#include <fstream>

using namespace ns3;

// Функции для трассировки окна перегрузки
std::ofstream bbrCwndFile("bbr_cwnd_trace.txt", std::ios::out | std::ios::trunc);
std::ofstream splineCwndFile("spline_cwnd_trace.txt", std::ios::out | std::ios::trunc);
void CwndTracerBbr(uint32_t oldCwnd, uint32_t newCwnd)
{
    bbrCwndFile << Simulator::Now().GetSeconds() << " " << newCwnd << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: BBR cwnd changed from "
              << oldCwnd << " to " << newCwnd << std::endl;
}
void CwndTracerSpline(uint32_t oldCwnd, uint32_t newCwnd)
{
    splineCwndFile << Simulator::Now().GetSeconds() << " " << newCwnd << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: SplineCcNew cwnd changed from "
              << oldCwnd << " to " << newCwnd << std::endl;
}

// Функции для трассировки RTT
std::ofstream bbrRttFile("bbr_rtt_trace.txt", std::ios::out | std::ios::trunc);
std::ofstream splineRttFile("spline_rtt_trace.txt", std::ios::out | std::ios::trunc);
void RttTracerBbr(Time oldRtt, Time newRtt)
{
    bbrRttFile << Simulator::Now().GetSeconds() << " " << newRtt.GetMilliSeconds() << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: BBR RTT changed from "
              << oldRtt.GetMilliSeconds() << "ms to " << newRtt.GetMilliSeconds() << "ms" << std::endl;
}
void RttTracerSpline(Time oldRtt, Time newRtt)
{
    splineRttFile << Simulator::Now().GetSeconds() << " " << newRtt.GetMilliSeconds() << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: SplineCcNew RTT changed from "
              << oldRtt.GetMilliSeconds() << "ms to " << newRtt.GetMilliSeconds() << "ms" << std::endl;
}

// Функции для трассировки pacing rate
std::ofstream bbrPacingFile("bbr_pacing_rate_trace.txt", std::ios::out | std::ios::trunc);
std::ofstream splinePacingFile("spline_pacing_rate_trace.txt", std::ios::out | std::ios::trunc);
void PacingRateTracerBbr(DataRate oldPacingRate, DataRate newPacingRate)
{
    bbrPacingFile.open("bbr_pacing_rate_trace.txt", std::ios::out | std::ios::app);
    bbrPacingFile << Simulator::Now().GetSeconds() << " " << newPacingRate.GetBitRate() / 1e6 << std::endl;
    bbrPacingFile.close();
    std::cout << Simulator::Now().GetSeconds() << "s: BBR pacing rate changed from "
              << oldPacingRate.GetBitRate() / 1e6 << " Mbps to " << newPacingRate.GetBitRate() / 1e6 << " Mbps" << std::endl;
}
void PacingRateTracerSpline(DataRate oldPacingRate, DataRate newPacingRate)
{
    splinePacingFile.open("spline_pacing_rate_trace.txt", std::ios::out | std::ios::app);
    splinePacingFile << Simulator::Now().GetSeconds() << " " << newPacingRate.GetBitRate() / 1e6 << std::endl;
    splinePacingFile.close();
    std::cout << Simulator::Now().GetSeconds() << "s: SplineCcNew pacing rate changed from "
              << oldPacingRate.GetBitRate() / 1e6 << " Mbps to " << newPacingRate.GetBitRate() / 1e6 << " Mbps" << std::endl;
}

// Функции для трассировки состояния TCP
std::ofstream bbrStateFile("bbr_cong_state_trace.txt", std::ios::out | std::ios::trunc);
std::ofstream splineStateFile("spline_cong_state_trace.txt", std::ios::out | std::ios::trunc);
void CongStateTracerBbr(TcpSocketState::TcpCongState_t oldState, TcpSocketState::TcpCongState_t newState)
{
    bbrStateFile << Simulator::Now().GetSeconds() << " " << newState << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: BBR cong state changed from "
              << oldState << " to " << newState << std::endl;
}
void CongStateTracerSpline(TcpSocketState::TcpCongState_t oldState, TcpSocketState::TcpCongState_t newState)
{
    splineStateFile << Simulator::Now().GetSeconds() << " " << newState << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: SplineCcNew cong state changed from "
              << oldState << " to " << newState << std::endl;
}

// Функция для трассировки получения пакетов
std::ofstream bbrRxFile("bbr_rx_trace.txt", std::ios::out | std::ios::trunc);
std::ofstream splineRxFile("spline_rx_trace.txt", std::ios::out | std::ios::trunc);
void PacketRxTracer(uint32_t flowId, Ptr<const Packet> packet, const Address& from)
{
    std::ofstream* rxFile = (flowId == 1) ? &bbrRxFile : &splineRxFile;
    *rxFile << Simulator::Now().GetSeconds() << " " << packet->GetSize() << std::endl;
    std::cout << Simulator::Now().GetSeconds() << "s: Flow " << flowId
              << " received packet of size " << packet->GetSize() << " from " << from << std::endl;
}

// Функция для подключения трассировки
void ConnectCwndTrace(Ptr<Application> app, bool isBbr, uint32_t flowId)
{
    Ptr<OnOffApplication> onOffApp = DynamicCast<OnOffApplication>(app);
    if (!onOffApp)
    {
        std::cout << Simulator::Now().GetSeconds() << "s: Failed to cast to OnOffApplication for flow " << flowId << "!" << std::endl;
        return;
    }

    Ptr<Socket> socket = onOffApp->GetSocket();
    Ptr<TcpSocketBase> tcpSocket = DynamicCast<TcpSocketBase>(socket);
    if (tcpSocket)
    {
        if (isBbr)
        {
            tcpSocket->TraceConnectWithoutContext("CongestionWindow", MakeCallback(&CwndTracerBbr));
            tcpSocket->TraceConnectWithoutContext("RTT", MakeCallback(&RttTracerBbr));
            tcpSocket->TraceConnectWithoutContext("PacingRate", MakeCallback(&PacingRateTracerBbr));
            tcpSocket->TraceConnectWithoutContext("CongState", MakeCallback(&CongStateTracerBbr));
            std::cout << Simulator::Now().GetSeconds() << "s: BBR Cwnd, RTT, PacingRate, CongState tracing enabled for flow " << flowId << "." << std::endl;
        }
        else
        {
            tcpSocket->TraceConnectWithoutContext("CongestionWindow", MakeCallback(&CwndTracerSpline));
            tcpSocket->TraceConnectWithoutContext("RTT", MakeCallback(&RttTracerSpline));
            tcpSocket->TraceConnectWithoutContext("PacingRate", MakeCallback(&PacingRateTracerSpline));
            tcpSocket->TraceConnectWithoutContext("CongState", MakeCallback(&CongStateTracerSpline));
            std::cout << Simulator::Now().GetSeconds() << "s: SplineCcNew Cwnd, RTT, PacingRate, CongState tracing enabled for flow " << flowId << "." << std::endl;
        }
    }
    else
    {
        std::cout << Simulator::Now().GetSeconds() << "s: Failed to get TCP socket for flow " << flowId << "!" << std::endl;
    }
}

int main(int argc, char *argv[])
{
    std::cout << "Starting simulation setup..." << std::endl;

    // Включение логирования
    LogComponentEnable("TcpBbr", LOG_LEVEL_INFO);
    LogComponentEnable("SplineCcNew", LOG_LEVEL_INFO);
    LogComponentEnable("TcpSocketBase", LOG_LEVEL_INFO);

    // Создание узлов: n0 (BBR client), n1 (BBR server), n2 (SplineCcNew client), n3 (SplineCcNew server)
    NodeContainer nodes;
    nodes.Create(4);
    std::cout << "Created 4 nodes: n0 (BBR client), n1 (BBR server), n2 (SplineCcNew client), n3 (SplineCcNew server)." << std::endl;

    // Настройка каналов точка-точка
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10000Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("50ms"));
    p2p.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("1000p"));

    // Канал 1: n0 (BBR client) ↔ n1 (BBR server)
    NetDeviceContainer devicesBbr = p2p.Install(nodes.Get(0), nodes.Get(1));
    std::cout << "Point-to-point channel configured for BBR (n0 ↔ n1, 10000Mbps, 50ms delay)." << std::endl;

    // Канал 2: n2 (SplineCcNew client) ↔ n3 (SplineCcNew server)
    NetDeviceContainer devicesSpline = p2p.Install(nodes.Get(2), nodes.Get(3));
    std::cout << "Point-to-point channel configured for SplineCcNew (n2 ↔ n3, 10000Mbps, 50ms delay)." << std::endl;

    // Установка интернет-стека
    InternetStackHelper stack;
    stack.Install(nodes);
    std::cout << "Internet stack installed on all nodes." << std::endl;

    // Настройка TrafficControlHelper
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::PieQueueDisc");
    tch.Install(devicesBbr);
    tch.Install(devicesSpline);
    std::cout << "Traffic control (PIE queue disc) installed on both channels." << std::endl;

    // Добавление потерь пакетов
    Ptr<RateErrorModel> errorModel = CreateObject<RateErrorModel>();
    errorModel->SetAttribute("ErrorRate", DoubleValue(0.00001)); // 0.01%
    devicesBbr.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel));
    devicesBbr.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel));
    devicesSpline.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel));
    devicesSpline.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel));
    std::cout << "Packet loss rate set to 0.01% on both channels." << std::endl;

    // Назначение IP-адресов
    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfacesBbr = address.Assign(devicesBbr);
    address.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer interfacesSpline = address.Assign(devicesSpline);
    std::cout << "IP addresses assigned: 10.1.1.0/24 for BBR, 10.1.2.0/24 for SplineCcNew." << std::endl;

    // Установка общих параметров TCP
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(10));
    Config::SetDefault("ns3::TcpSocket::DelAckCount", UintegerValue(2));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1000000)); // 1 MB
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1000000)); // 1 MB
    std::cout << "TCP parameters set: segment size 1448 bytes, initial cwnd 10." << std::endl;

    // Настройка сервера (PacketSink) для BBR
    uint16_t portBbr = 8080;
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TypeId::LookupByName("ns3::TcpBbr")));
    PacketSinkHelper sinkBbr("ns3::TcpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), portBbr));
    ApplicationContainer sinkAppBbr = sinkBbr.Install(nodes.Get(1)); // n1
    sinkAppBbr.Start(Seconds(0.0));
    sinkAppBbr.Stop(Seconds(60.0));
    std::cout << "PacketSink for BBR installed on node 1, port " << portBbr << "." << std::endl;

    // Настройка клиента (OnOff) для BBR
    OnOffHelper clientBbr("ns3::TcpSocketFactory",
                          Address(InetSocketAddress(interfacesBbr.GetAddress(1), portBbr)));
    clientBbr.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=60]"));
    clientBbr.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    clientBbr.SetAttribute("DataRate", StringValue("5000Mbps"));
    ApplicationContainer clientAppBbr = clientBbr.Install(nodes.Get(0)); // n0
    clientAppBbr.Start(Seconds(1.0));
    clientAppBbr.Stop(Seconds(60.0));
    std::cout << "OnOff client for BBR installed on node 0, data rate 5000Mbps." << std::endl;

    // Настройка сервера (PacketSink) для SplineCcNew
    uint16_t portSpline = 8081;
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TypeId::LookupByName("ns3::SplineCcNew")));
    PacketSinkHelper sinkSpline("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), portSpline));
    ApplicationContainer sinkAppSpline = sinkSpline.Install(nodes.Get(3)); // n3
    sinkAppSpline.Start(Seconds(0.0));
    sinkAppSpline.Stop(Seconds(60.0));
    std::cout << "PacketSink for SplineCcNew installed on node 3, port " << portSpline << "." << std::endl;

    // Настройка клиента (OnOff) для SplineCcNew
    OnOffHelper clientSpline("ns3::TcpSocketFactory",
                             Address(InetSocketAddress(interfacesSpline.GetAddress(1), portSpline)));
    clientSpline.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=60]"));
    clientSpline.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    clientSpline.SetAttribute("DataRate", StringValue("5000Mbps"));
    ApplicationContainer clientAppSpline = clientSpline.Install(nodes.Get(2)); // n2
    clientAppSpline.Start(Seconds(1.0));
    clientAppSpline.Stop(Seconds(60.0));
    std::cout << "OnOff client for SplineCcNew installed on node 2, data rate 5000Mbps." << std::endl;

    // Настройка трассировки получения пакетов
    Ptr<PacketSink> sinkAppPtrBbr = DynamicCast<PacketSink>(sinkAppBbr.Get(0));
    if (sinkAppPtrBbr)
    {
        sinkAppPtrBbr->TraceConnectWithoutContext("Rx", MakeBoundCallback(&PacketRxTracer, 1));
        std::cout << "PacketSink tracing enabled for BBR (flow 1)." << std::endl;
    }
    else
    {
        std::cout << "Failed to get PacketSink application for BBR!" << std::endl;
    }

    Ptr<PacketSink> sinkAppPtrSpline = DynamicCast<PacketSink>(sinkAppSpline.Get(0));
    if (sinkAppPtrSpline)
    {
        sinkAppPtrSpline->TraceConnectWithoutContext("Rx", MakeBoundCallback(&PacketRxTracer, 2));
        std::cout << "PacketSink tracing enabled for SplineCcNew (flow 2)." << std::endl;
    }
    else
    {
        std::cout << "Failed to get PacketSink application for SplineCcNew!" << std::endl;
    }

    // Планирование подключения трассировки
    Simulator::Schedule(Seconds(1.01), &ConnectCwndTrace, clientAppBbr.Get(0), true, 1);
    Simulator::Schedule(Seconds(1.01), &ConnectCwndTrace, clientAppSpline.Get(0), false, 2);
    std::cout << "Scheduled cwnd, RTT, pacing rate, and cong state tracing at 1.01s for both flows." << std::endl;

    // Настройка FlowMonitor
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();
    std::cout << "FlowMonitor installed." << std::endl;

    // Запуск симуляции
    Simulator::Stop(Seconds(60.0));
    std::cout << "Starting simulation..." << std::endl;
    Simulator::Run();
    std::cout << "Simulation finished." << std::endl;

    // Сбор и сохранение статистики
    std::cout << "Writing FlowMonitor results to bbr_spline.flowmon..." << std::endl;
    monitor->CheckForLostPackets();
    monitor->SerializeToXmlFile("bbr_spline.flowmon", true, true);

    std::cout << "Simulation completed successfully." << std::endl;
    Simulator::Destroy();
    return 0;
}
