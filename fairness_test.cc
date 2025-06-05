#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-layout-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/log.h"
#include <iostream>
#include <string>

using namespace ns3;

static double packetSize = 1448.0;

static void CwndTracer(Ptr<OutputStreamWrapper> stream, uint32_t oldval, uint32_t newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval / packetSize << std::endl;
}

static void RttTracer(Ptr<OutputStreamWrapper> stream, Time oldval, Time newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval.GetMilliSeconds() << std::endl;
}

static void RtoTracer(Ptr<OutputStreamWrapper> stream, Time oldval, Time newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval.GetSeconds() << std::endl;
}

static void InFlightTracer(Ptr<OutputStreamWrapper> stream, uint32_t oldval, uint32_t newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval / packetSize << std::endl;
}

static void SsThreshTracer(Ptr<OutputStreamWrapper> stream, uint32_t oldval, uint32_t newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval << std::endl;
}

static void BytesInQueueTrace(Ptr<OutputStreamWrapper> stream, uint32_t oldVal, uint32_t newVal){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newVal / packetSize << std::endl;
}

static void NextTxTracer(Ptr<OutputStreamWrapper> stream, SequenceNumber32 oldval, SequenceNumber32 newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval << std::endl;
}

static void PacingRateTracer(Ptr<OutputStreamWrapper> stream, DataRate oldval, DataRate newval){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << newval.GetBitRate() / 1e6 << std::endl;
}

static void RxTracer(Ptr<OutputStreamWrapper> stream, Ptr<const Packet> packet, const Address &from){
    *stream->GetStream() << Simulator::Now().GetSeconds() << " " << packet->GetSize() << std::endl;
}

void TraceCwnd(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/CongestionWindow", MakeBoundCallback(&CwndTracer, stream));
}

void TraceRtt(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/RTT", MakeBoundCallback(&RttTracer, stream));
}

void TraceRto(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/RTO", MakeBoundCallback(&RtoTracer, stream));
}

void TraceInflight(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/BytesInFlight", MakeBoundCallback(&InFlightTracer, stream));
}

void TraceSsThresh(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/SlowStartThreshold", MakeBoundCallback(&SsThreshTracer, stream));
}

void TraceNextTx(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/NextTxSequence", MakeBoundCallback(&NextTxTracer, stream));
}

void TracePacingRate(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/$ns3::TcpL4Protocol/SocketList/0/PacingRate", MakeBoundCallback(&PacingRateTracer, stream));
}

void TraceRx(std::string file_name, uint16_t nodeId){
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(file_name);
    Config::ConnectWithoutContext("/NodeList/"+std::to_string(nodeId)+"/ApplicationList/0/$ns3::PacketSink/Rx", MakeBoundCallback(&RxTracer, stream));
}

static void TraceThroughput(Ptr<FlowMonitor> monitor, std::vector<Ptr<OutputStreamWrapper>> stream, std::vector<uint64_t> prev){
    FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats();
    for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin(); i != stats.end(); ++i){
        if(prev.size() < i->first){
            prev.push_back(0);
        }
        *stream[i->first-1]->GetStream() << Simulator::Now().GetSeconds() << " " << (i->second.txBytes - prev[i->first-1]) * 8 * 10 / 1000000.0 << std::endl;
        prev[i->first-1] = i->second.txBytes;
    }
    Simulator::Schedule(Seconds(0.1), &TraceThroughput, monitor, stream, prev);
}

static void TracePacketLoss(Ptr<FlowMonitor> monitor, std::vector<Ptr<OutputStreamWrapper>> stream, std::vector<uint32_t> prev){
    FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats();
    for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin(); i != stats.end(); ++i){
        if(prev.size() < i->first){
            prev.push_back(0);
        }
        *stream[i->first-1]->GetStream() << Simulator::Now().GetSeconds() << " " << i->second.lostPackets - prev[i->first-1] << std::endl;
        prev[i->first-1] = i->second.lostPackets;
    }
    Simulator::Schedule(Seconds(0.1), &TracePacketLoss, monitor, stream, prev);
}

static void TraceTime(){
    std::cout << Simulator::Now().GetSeconds() << "s" << std::endl;
    Simulator::Schedule(Seconds(10), &TraceTime);
}

int main(int argc, char* argv[]){
    uint32_t nLeaf = 2;
    uint32_t startTime = 0;
    uint32_t stopTime = 60;
    double errorRate = 0.00;
    std::string routerRate = "50Mbps";
    std::string routerDelay = "80ms";
    std::string leafRate = "100Mbps";
    std::string leafDelay = "10ms";
    std::string queueDisc = "ns3::PfifoFastQueueDisc";
    double queueSize = 1.5;
    std::vector<std::string> tcpVariant = {"ns3::TcpBbr", "ns3::SplineCcNew"};

    CommandLine cmd(__FILE__);
    cmd.AddValue("nLeaf", "Number of left and right side leaf nodes", nLeaf);
    cmd.Parse(argc, argv);

    // Set output directory
    std::string dir = "/home/ns3/NS3-bbr/build/";
    AsciiTraceHelper ascii;
    Ptr<OutputStreamWrapper> logStream = ascii.CreateFileStream(dir + "log.txt");

    // Set up some default values
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(2097152));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(2097152));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(100));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(packetSize));
    if(queueSize != 0){
        DataRate b(routerRate);
        Time l(leafDelay);
        Time r(routerDelay);
        queueSize *= (b.GetBitRate() / 8) * ((r + l*2) * 2).GetSeconds();
        Config::SetDefault(queueDisc + "::MaxSize", QueueSizeValue(QueueSize(PACKETS, queueSize/packetSize)));
        Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", QueueSizeValue(QueueSize(PACKETS, queueSize/packetSize)));
    }
    *logStream->GetStream() << "QueueDisc: " << queueDisc << " MaxSize: " << queueSize/packetSize << std::endl;

    // Here we use RateErrorModel with packet error rate
    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    uv->SetStream(50);
    RateErrorModel error_model;
    error_model.SetRandomVariable(uv);
    error_model.SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
    error_model.SetRate(errorRate);
    *logStream->GetStream() << "ErrorRate: " << errorRate << std::endl;

    // Create the point-to-point link helpers
    PointToPointHelper pointToPointRouter;
    pointToPointRouter.SetDeviceAttribute("DataRate", StringValue(routerRate));
    pointToPointRouter.SetChannelAttribute("Delay", StringValue(routerDelay));
    pointToPointRouter.SetDeviceAttribute("ReceiveErrorModel", PointerValue(&error_model));
    PointToPointHelper pointToPointLeaf;
    pointToPointLeaf.SetDeviceAttribute("DataRate", StringValue(leafRate));
    pointToPointLeaf.SetChannelAttribute("Delay", StringValue(leafDelay));
    *logStream->GetStream() << "RouterRate: " << routerRate << " RouterDelay: " << routerDelay << " LeafRate: " << leafRate << " LeafDelay: " << leafDelay << std::endl;

    // Create the dumbbell topology
    PointToPointDumbbellHelper d(nLeaf, pointToPointLeaf, nLeaf, pointToPointLeaf, pointToPointRouter);
    *logStream->GetStream() << "nleaf: " << nLeaf << " startTime: " << startTime << " stopTime: " << stopTime << std::endl;

    // Install Stack
    InternetStackHelper stack;
    d.InstallStack(stack);

    // Configure the root queue discipline
    TrafficControlHelper tch;
    tch.SetRootQueueDisc(queueDisc);
    d.InstallControl(tch);

    // Assign IP Addresses
    d.AssignIpv4Addresses(Ipv4AddressHelper("10.1.1.0", "255.255.255.0"), Ipv4AddressHelper("10.2.1.0", "255.255.255.0"), Ipv4AddressHelper("10.3.1.0", "255.255.255.0"));

    // Set up congestion control
    for (uint16_t i = 0; i < Min(tcpVariant.size(), nLeaf); i++){
        std::stringstream nodeId;
        nodeId << d.GetLeft(i)->GetId();
        std::string node = "/NodeList/" + nodeId.str() + "/$ns3::TcpL4Protocol/SocketType";
        TypeId tid = TypeId::LookupByName(tcpVariant[i]);
        Config::Set(node, TypeIdValue(tid));
        *logStream->GetStream() << "Node: " << nodeId.str() << " TcpVariant: " << tcpVariant[i] << std::endl;
    }

    // Install application on sender
    uint16_t port = 9;
    BulkSendHelper source("ns3::TcpSocketFactory", Address());
    source.SetAttribute("MaxBytes", UintegerValue(0));
    ApplicationContainer sourceApps;
    for (uint16_t i = 0; i < nLeaf; i++){
        AddressValue remoteAddress(InetSocketAddress(d.GetRightIpv4Address(i), port));
        source.SetAttribute("Remote", remoteAddress);
        sourceApps.Add(source.Install(d.GetLeft(i)));
    }
    sourceApps.Start(Seconds(startTime));
    sourceApps.Stop(Seconds(stopTime));

    // Install application on the receiver
    PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps;
    for (uint16_t i = 0; i < nLeaf; i++){
        sinkApps.Add(sink.Install(d.GetRight(i)));
    }
    sinkApps.Start(Seconds(startTime));
    sinkApps.Stop(Seconds(stopTime));

    // Set the bounding box for animation
    d.BoundingBox(1, 1, 100, 100);

    // Set up the actual simulation
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Schedule tracing
    std::vector<std::string> protocolNames = {"bbr", "spline"};
    for(uint16_t i = 0; i < nLeaf; i++){
        std::string prefix = protocolNames[i];
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceCwnd, dir + prefix + "_cwnd_trace.txt", i+2);
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceRtt, dir + prefix + "_rtt_trace.txt", i+2);
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TracePacingRate, dir + prefix + "_pacing_rate_trace.txt", i+2);
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceRx, dir + prefix + "_rx_trace.txt", d.GetRight(i)->GetId());
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceInflight, dir + prefix + "_inflight_trace.txt", i+2);
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceRto, dir + prefix + "_rto_trace.txt", i+2);
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceSsThresh, dir + prefix + "_ssthresh_trace.txt", i+2);
        Simulator::Schedule(Seconds(startTime) + NanoSeconds(1), &TraceNextTx, dir + prefix + "_nexttx_trace.txt", i+2);
    }

    // Schedule throughput and packet loss tracing
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();
    std::vector<Ptr<OutputStreamWrapper>> throughputStreams;
    std::vector<Ptr<OutputStreamWrapper>> packetLossStreams;
    for(uint16_t i = 0; i < nLeaf*2; i++){
        std::string prefix = protocolNames[i % nLeaf];
        throughputStreams.push_back(ascii.CreateFileStream(dir + prefix + "_throughput_trace-" + std::to_string(i) + ".txt"));
        packetLossStreams.push_back(ascii.CreateFileStream(dir + prefix + "_packetloss_trace-" + std::to_string(i) + ".txt"));
    }
    Simulator::Schedule(Seconds(startTime), &TraceThroughput, monitor, throughputStreams, std::vector<uint64_t>());
    Simulator::Schedule(Seconds(startTime), &TracePacketLoss, monitor, packetLossStreams, std::vector<uint32_t>());

    // Schedule queue size tracing
    Ptr<Queue<Packet>> queue = StaticCast<PointToPointNetDevice>(d.GetRouterDevice().Get(0))->GetQueue();
    Ptr<OutputStreamWrapper> queueSizeStream = ascii.CreateFileStream(dir + "queue_size_trace.txt");
    queue->TraceConnectWithoutContext("BytesInQueue", MakeBoundCallback(&BytesInQueueTrace, queueSizeStream));

    // Run simulation
    std::cout << "Running" << std::endl;
    Simulator::Schedule(Seconds(startTime+10), &TraceTime);
    Simulator::Stop(Seconds(stopTime) + TimeStep(1));
    Simulator::Run();
    Simulator::Destroy();

    // Flow monitor output
    flowmon.SerializeToXmlFile(dir + "bbr_spline.flowmon", true, true);
    FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats();
    for(std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin(); i != stats.end(); ++i){
        *logStream->GetStream() << "Flow: " << i->first << " Throughput: " << ((double) i->second.rxBytes * 8.0) / (double) (i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds()) / 1024 / 1024 << std::endl;
    }
    return 0;
}
