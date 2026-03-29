// SPDX-License-Identifier: GPL-2.0-only
#include <iostream>
#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-alps-tag.h"


namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbSwitch);
NS_LOG_COMPONENT_DEFINE("UbSwitch");


/*-----------------------------------------UbSwitchNode----------------------------------------------*/

TypeId UbSwitch::GetTypeId (void)
{
    static TypeId tid = TypeId ("ns3::UbSwitch")
        .SetParent<Object> ()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbSwitch> ()
        .AddAttribute("FlowControl",
                      "The flow control mechanism to use (NONE, CBFC, CBFC_SHARED, or PFC).",
                      EnumValue(FcType::NONE),
                      MakeEnumAccessor<FcType>(&UbSwitch::m_flowControlType),
                      MakeEnumChecker(FcType::NONE, "NONE",
                                      FcType::CBFC, "CBFC",
                                      FcType::CBFC_SHARED_CRD, "CBFC_SHARED",
                                      FcType::PFC, "PFC"))
        .AddAttribute("VlScheduler",
                      "VL inter-scheduling algorithm (SP or DWRR).",
                      EnumValue(SP),
                      MakeEnumAccessor<VlScheduler>(&UbSwitch::m_vlScheduler),
                      MakeEnumChecker(SP, "SP",
                                      DWRR, "DWRR"))
        .AddTraceSource("LastPacketTraversesNotify",
                        "Last Packet Traverses, NodeId",
                        MakeTraceSourceAccessor(&UbSwitch::m_traceLastPacketTraversesNotify),
                        "ns3::UbSwitch::LastPacketTraversesNotify");
    return tid;
}
/**
 * @brief Init UbNode, create algorithm, queueManager, fc and so on
 */
void UbSwitch::Init()
{
    auto node = GetObject<Node>();
    m_portsNum = node->GetNDevices();
    // alg init
    switch (m_vlScheduler) {
        case DWRR:
            m_allocator = CreateObject<UbDwrrAllocator>();
            break;
        case SP:
        default:
            m_allocator = CreateObject<UbRoundRobinAllocator>();
            break;
    }
    m_allocator->SetNodeId(node->GetId());
    m_allocator->Init();
    VoqInit();
    RegisterVoqsWithAllocator();

    // queueManager init
    m_queueManager = CreateObject<UbQueueManager>();
    m_queueManager->SetVLNum(m_vlNum);
    m_queueManager->SetPortsNum(m_portsNum);
    m_queueManager->Init();

    InitNodePortsFlowControl();
    //PrintPortThresholdSummary();
    m_routingProcess = CreateObject<UbRoutingProcess>();
    m_routingProcess->SetNodeId(node->GetId());
    m_Ipv4Addr = utils::NodeIdToIp(node->GetId());
}

void UbSwitch::PrintPortThresholdSummary()
{
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr || m_queueManager == nullptr) {
        return;
    }

    const char* nodeType = (m_nodeType == UB_SWITCH) ? "SWITCH" : "DEVICE";
    const uint32_t perQueueLimitBytes = m_queueManager->GetBufferSizePerQueue();
    const uint64_t perPortLimitBytes = static_cast<uint64_t>(perQueueLimitBytes) * m_vlNum;

    NS_LOG_UNCOND("UB_PORT_THRESH_INIT|node=" << node->GetId()
                  << "|nodeType=" << nodeType
                  << "|ports=" << m_portsNum
                  << "|vlNum=" << m_vlNum
                  << "|perQueueDropThresholdBytes=" << perQueueLimitBytes
                  << "|perPortCapacityBytes=" << perPortLimitBytes
                  << "|flowControl=" << static_cast<uint32_t>(m_flowControlType));

    for (uint32_t portId = 0; portId < m_portsNum; ++portId) {
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
        if (port == nullptr) {
            continue;
        }

        if (IsPFCEnable()) {
            NS_LOG_UNCOND("UB_PORT_THRESH_INIT|node=" << node->GetId()
                          << "|nodeType=" << nodeType
                          << "|port=" << portId
                          << "|pfcUpThresholdBytes=" << port->GetPfcUpThld()
                          << "|pfcLowThresholdBytes=" << port->GetPfcLowThld());
        } else {
            NS_LOG_UNCOND("UB_PORT_THRESH_INIT|node=" << node->GetId()
                          << "|nodeType=" << nodeType
                          << "|port=" << portId
                          << "|pfcUpThresholdBytes=N/A"
                          << "|pfcLowThresholdBytes=N/A");
        }

        for (uint32_t pri = 0; pri < m_vlNum; ++pri) {
            NS_LOG_UNCOND("UB_PORT_THRESH_INIT|node=" << node->GetId()
                          << "|nodeType=" << nodeType
                          << "|port=" << portId
                          << "|priority=" << pri
                          << "|queueCapacityBytes=" << perQueueLimitBytes
                          << "|dropThresholdBytes=" << perQueueLimitBytes);
        }
    }
}

void UbSwitch::DoDispose()
{
    m_queueManager = nullptr;
    m_congestionCtrl = nullptr;
    m_allocator = nullptr;
    m_voq.clear();
    m_routingProcess = nullptr;
}

/**
 * @brief Init flow control for each port
 */
void UbSwitch::InitNodePortsFlowControl()
{
    NS_LOG_DEBUG("[UbSwitch InitNodePortsFlowControl] m_portsNum: " << m_portsNum
                << " m_flowControlType: " << static_cast<int>(m_flowControlType));

    for (uint32_t pidx = 0; pidx < m_portsNum; pidx++) {
        Ptr<UbPort> port = DynamicCast<ns3::UbPort>(GetObject<Node>()->GetDevice(pidx));
        port->CreateAndInitFc(m_flowControlType);
    }
}

/**
 * @brief 将初始化后的vop放入调度算法中
 */
void UbSwitch::RegisterVoqsWithAllocator()
{
    for (uint32_t i = 0; i < m_portsNum; i++) {
        for (uint32_t j = 0; j < m_vlNum; j++) {
            for (uint32_t k = 0 ; k < m_portsNum; k++) { // voq
                auto ingressQ = m_voq[i][j][k];
                m_allocator->RegisterUbIngressQueue(ingressQ, i, j);
            }
        }
    }
}

/**
 * @brief 将tp放入调度算法中
 */
void UbSwitch::RegisterTpWithAllocator(Ptr<UbIngressQueue> tp, uint32_t outPort, uint32_t priority)
{
    if ((outPort >= m_portsNum) || (priority >= m_vlNum)) {
        NS_ASSERT_MSG(0, "Invalid indices (outPort, priority)!");
    }
    NS_LOG_DEBUG("[UbSwitch RegisterTpWithAllocator] TP: outPortIdx: " << outPort
                 << "priorityIdx: " << priority << "outPort: " << outPort);
    tp->SetOutPortId(outPort);
    tp->SetInPortId(outPort); // tp uses outPort as inPort, since tp has no inPort
    tp->SetIngressPriority(priority);
    m_allocator->RegisterUbIngressQueue(tp, outPort, priority);
}

/**
 * @brief 将tp从调度算法中删除
 */
void UbSwitch::RemoveTpFromAllocator(Ptr<UbIngressQueue> tp)
{
    uint32_t outPort = tp->GetOutPortId();
    uint32_t priority = tp->GetIngressPriority();
    m_allocator->UnregisterUbIngressQueue(tp, outPort, priority);
}

UbSwitch::UbSwitch()
{
}
UbSwitch::~UbSwitch()
{
}

Ptr<UbSwitchAllocator> UbSwitch::GetAllocator()
{
    return m_allocator;
}

/**
 * @brief init voq
 */
void UbSwitch::VoqInit()
{
    uint32_t outPortIdx = 0;
    uint32_t priorityIdx = 0;
    uint32_t inPortIdx = 0;
    m_voq.resize(m_portsNum);
    for (auto &i : m_voq) {
        priorityIdx = 0;
        i.resize(m_vlNum);
        for (auto &j : i) {
            inPortIdx = 0;
            for (uint32_t k = 0; k < m_portsNum; k++) {
                auto q = CreateObject<UbPacketQueue>();
                q->SetOutPortId(outPortIdx);
                q->SetIngressPriority(priorityIdx);
                q->SetInPortId(inPortIdx); // tp不使用inport
                q->SetInPortId(k);
                j.push_back(q);
                inPortIdx++;
            }
            priorityIdx++;
        }
        outPortIdx++;
    }
}

/**
 * @brief push packet into voq
 */
void UbSwitch::PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        NS_ASSERT_MSG(0, "Invalid VOQ indices (outPort, priority, inPort)!");
    }
    m_voq[outPort][priority][inPort]->Push(p);
}

bool UbSwitch::IsValidVoqIndices(uint32_t outPort, uint32_t priority, uint32_t inPort, uint32_t portsNum, uint32_t vlNum)
{
    return outPort < portsNum && priority < vlNum && inPort < portsNum;
}

UbPacketType_t UbSwitch::GetPacketType(Ptr<Packet> packet)
{
    UbDatalinkHeader dlHeader;
    packet->PeekHeader(dlHeader);
    if (dlHeader.IsControlCreditHeader())
        return UB_CONTROL_FRAME;
    if (dlHeader.IsPacketIpv4Header())
        return UB_URMA_DATA_PACKET;
    if (dlHeader.IsPacketUbMemHeader())
        return UB_LDST_DATA_PACKET;
    return UNKOWN_TYPE;
}

/**
 * @brief Receive packet from port. Node handle packet
 */
void UbSwitch::SwitchHandlePacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // 帧类型判断
    auto packetType = GetPacketType(packet);
    switch (packetType) {
        case UB_CONTROL_FRAME:
            port->m_flowControl->HandleReceivedControlPacket(packet);
            break;
        case UB_URMA_DATA_PACKET:
            HandleURMADataPacket(port, packet);
            break;
        case UB_LDST_DATA_PACKET:
            HandleLdstDataPacket(port, packet);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Packet Type!");
    }
    return;
}

/**
 * @brief Sink control frame
 */
void UbSwitch::SinkControlFrame(Ptr<UbPort> port, Ptr<Packet> packet)
{
    if (IsCBFCSharedEnable()) {
        auto flowControl = DynamicCast<UbCbfcSharedCredit>(port->GetFlowControl());
        flowControl->CbfcSharedRestoreCrd(packet);
    } else if (IsCBFCEnable()) {
        auto flowControl = DynamicCast<UbCbfc>(port->GetFlowControl());
        flowControl->CbfcRestoreCrd(packet);
    } else if (IsPFCEnable()) {
        auto flowControl = DynamicCast<UbPfc>(port->GetFlowControl());
        flowControl->UpdatePfcStatus(packet);
    }

    return;
}

/**
 * @brief Handle URMA type data packet
 */
void UbSwitch::HandleURMADataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // Parse headers once for efficient reuse
    ParsedURMAHeaders headers;
    ParseURMAPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkTpDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Handle Ldst type data packet
 */
void UbSwitch::HandleLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // Parse headers once for efficient reuse
    ParsedLdstHeaders headers;
    ParseLdstPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkLdstDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Sink URMA type data packet
 */
bool UbSwitch::SinkTpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
    NS_LOG_DEBUG("[UbPort recv] Psn: " << headers.transportHeader.GetPsn());
    Ipv4Mask mask("255.255.255.0");

    // Forward
    if (!utils::IsInSameSubnet(headers.ipv4Header.GetDestination(), GetNodeIpv4Addr(), mask)) {
        return false;
    }
    // Sink
    NS_LOG_DEBUG("[UbPort recv] Pkt tb is local");
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->m_flowControl->HandleReceivedPacket(packet);
    }

    uint32_t dstTpn = headers.transportHeader.GetDestTpn();
    //std::cout << "NodeId:" << GetObject<Node>()->GetId() << " recv packet with dstTpn: " << dstTpn << std::endl;
    auto targetTp = GetObject<UbController>()->GetTpByTpn(dstTpn);
    if (targetTp == nullptr) {
        if (GetObject<UbController>()->GetTpConnManager()->IsTpRemoveMode()) {
            NS_LOG_WARN("Auto remove tp mode, drop this packet.");
            return true;
        } else {
            NS_ASSERT_MSG(0, "Port Cannot Get Tp By Tpn!");
        }
    }
    if (headers.transportHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)
        || headers.transportHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH)) {
        NS_LOG_DEBUG("[UbPort recv] is ACK");
        UbDatalinkPacketHeader tempDlHeader;
        UbNetworkHeader tempNetHeader;
        Ipv4Header tempIpv4Header;
        UdpHeader tempUdpHeader;
        packet->RemoveHeader(tempDlHeader);
        packet->RemoveHeader(tempNetHeader);
        packet->RemoveHeader(tempIpv4Header);
        packet->RemoveHeader(tempUdpHeader);
        //发送端接收ACK
        if(m_routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS){
           targetTp->RecvTpAckForAlps(packet);     
        }
        else{
           targetTp->RecvTpAck(packet);   
            }
        
    } else {
        //接收端接收数据包
         if(m_routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS){
            targetTp->RecvDataPacketForAlps(packet);
        }
        else{
           targetTp->RecvDataPacket(packet);
        }
        
    }
    return true;
}

/**
 * @brief Sink Ldst type data packet
 */
bool UbSwitch::SinkLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers)
{
    // Store/load request: DLH cNTH cTAH(0x03/0x06) [cMAETAH] Payload
    // Store/load response: DLH cNTH cATAH(0x11/0x12) Payload
    NS_LOG_DEBUG("[UbPort recv] ub ldst frame");
    uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
    uint32_t dnode = utils::Cna16ToNodeId(dCna);
    // Forward
    if (dnode != GetObject<Node>()->GetId()) {
        return false;
    }
    // Sink Packet
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->m_flowControl->HandleReceivedPacket(packet);
    }

    auto ldstApi = GetObject<Node>()->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
    NS_ASSERT_MSG(ldstApi != nullptr, "UbLdstApi can not be nullptr!");

    uint8_t type = headers.dummyTransactionHeader.GetTaOpcode();
    // 数据包
    if (type == (uint8_t)TaOpcode::TA_OPCODE_WRITE || type == (uint8_t)TaOpcode::TA_OPCODE_READ) {
        ldstApi->RecvDataPacket(packet);
    } else if (type == (uint8_t)TaOpcode::TA_OPCODE_TRANSACTION_ACK ||
               type == (uint8_t)TaOpcode::TA_OPCODE_READ_RESPONSE) {
        ldstApi->RecvResponse(packet);
        NS_LOG_DEBUG("ldst packet is ack!");
    } else {
        NS_ASSERT_MSG(0, "packet Ta Op code is wrong!");
    }
    return true;
}

/**
 * @brief Forward URMA data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
      //std::cout << "NodeId:" << GetObject<Node>()->GetId() << "1111111111111111111111111111111111111111" << headers.ipv4Header.GetDestination() << std::endl;
    // ALPS,则调用ALPS专用接口
    if (m_routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS) {
       //  std::cout << "NodeId:" << GetObject<Node>()->GetId() << "22222222222222222222222222222222222222222" << headers.ipv4Header.GetDestination() << std::endl;
        ForwardDataPacketAlps(port, packet, headers);
        return;
    }


    // Log packet traversal
    LastPacketTraversesNotify(GetObject<Node>()->GetId(), headers.transportHeader);

    // Get routing key from parsed headers
    RoutingKey rtKey;
    GetURMARoutingKey(headers, rtKey);

    // Route
    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    Ipv4Address dstIp(rtKey.dip);
    // uint32_t nodeId = GetObject<Node>()->GetId();
    // std::cout << "NodeId:" << nodeId << " inPort:" << port->GetIfIndex() << " selects outPort: " << outPort << " for packet with dip: " << dstIp << std::endl;
    if (outPort < 0) {
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        UbTransportChannel::s_totalSwitchRouteMissDrops++;
        return;
    }

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL(); // NOTE, ACK SET
    uint32_t pSize = packet->GetSize();

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!1");
        UbTransportChannel::s_totalSwitchInPortDropsNonAlps++;
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

/**
 * @brief Forward URMA data packet (headers already parsed), ALPS专用接口，根据ALPSTag中的路径ID选择出端口
 */
void UbSwitch::ForwardDataPacketAlps(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
   
    // Log packet traversal
    LastPacketTraversesNotify(GetObject<Node>()->GetId(), headers.transportHeader);

     // 检查是否为 ACK 包
    bool isAck = (headers.transportHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH) ||
                  headers.transportHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH));
    // Route: 根据ALPS Tag中的路径ID选择出端口
    UbAlpsTag alpsTag;
    if (!packet->PeekPacketTag(alpsTag)) {
        NS_LOG_WARN("ALPS Tag not found! Packet Dropped!");
        UbTransportChannel::s_totalSwitchAlpsTagMissingDrops++;
        return;
    }
    int outPort = m_routingProcess->GetOutPort(alpsTag);
    if (outPort < 0) {
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        UbTransportChannel::s_totalSwitchRouteMissDrops++;
        return;
    }


    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex(); // Index从0开始
    //uint8_t priority = headers.datalinkPacketHeader.GetPacketVL(); // NOTE, ACK SET in UbDataLink::GenPacketHeader()
 //================================================================================   
      
    // 🔥 关键修改：如果是 ACK 包，提升到高优先级（例如优先级 0）
    uint8_t priority = isAck ? 0 : headers.datalinkPacketHeader.GetPacketVL();
    
    // 可选：打印调试信息
    if (isAck) {
        //std::cout << "交换机NodeId:" << GetObject<Node>()->GetId() << "将ACK转发，从端口："<<outPort<<std::endl;
        //std::cout << "ACK大小：" << packet->GetSize() << std::endl;
        //添加对数据包出端口排队时延的计算
        //获取ACK对应数据包转发路径pid,然后获取数据包发出端口。
        uint32_t Packet_pid =UbRoutingProcess::GetReservePid(alpsTag.GetPathId());
        uint32_t Packet_outPort = m_routingProcess->GetOutPort(Packet_pid);
        uint64_t  queueingDelayNanoSeconds =  CalculatePacketQueueDelay(Packet_outPort);
        //获取·ACK转发时延
         Ptr<Node> node = GetObject<Node>();
         Ptr<UbSwitch> ubSwitch = node->GetObject<UbSwitch>();
         Ptr<UbPort> outPortDevice = DynamicCast<UbPort>(node->GetDevice(Packet_outPort));
          DataRate portRate = outPortDevice->GetDataRate();
          uint64_t  ackDelayNanoSeconds =  static_cast<double>((ACK_SIZE )* 8) / (portRate.GetBitRate()/1e9);
          
        Time originalTime = alpsTag.GetTimeStamp();
        Time queueingDelay = NanoSeconds(queueingDelayNanoSeconds-ackDelayNanoSeconds); // 从总排队+转发时延中减去ACK转发时延，得到数据包的排队时延

        Time newTime = originalTime - queueingDelay;
        alpsTag.SetTimeStamp(newTime);
        packet->ReplacePacketTag(alpsTag); // 更新ALPS Tag

    }
    else{
        if(true){
             //2026.3.21  看看rack1 layer1的交换机256输入端口0 1 2 3和257的输入端口4 5 6 7收到的数据包数量，看看ACK包和数据包的数量关系。
           record_packet_num( inPort);
       // std::cout << "交换机NodeId:" << GetObject<Node>()->GetId() << "将数据包转发，从端口："<<outPort<<std::endl;
          
    }
          //std::cout << "数据包大小：" << packet->GetSize() << std::endl;
    }

//==================================================================================
    uint32_t pSize = packet->GetSize();

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!2");
        UbTransportChannel::s_totalSwitchDropedPkts++;
        return;
    }

    // Update ALPS Tag for downstream switches
    alpsTag.SetHopCount(alpsTag.GetHopCount() + 1); // ALPS Tag中记录已经过的hop数，供ALPS算法使用
    packet->ReplacePacketTag(alpsTag); // 更新ALPS Tag

    SendPacket(packet, inPort, outPort, priority);
}
void UbSwitch::record_packet_num( uint32_t inports){
    static std::map<uint32_t, uint32_t> packet_num;
    static Time last_print_time = Time(0);
    uint32_t nodeId = GetObject<Node>()->GetId();
    if(nodeId!=256&&nodeId!=257){
        return;
    }
    if (packet_num.find(nodeId) == packet_num.end()) {
        packet_num[nodeId] = 0;
    }
    //if(inports>=0+4*(nodeId-256)&&inports<=3+4*(nodeId-256)){
          packet_num[nodeId]++;
    //}
  Time current_time = Simulator::Now();
    if ((current_time - last_print_time).GetMicroSeconds() >= 10) {
        for(const auto& entry : packet_num) { 
            std::cout << "[" << current_time.GetSeconds() << "s] Node " <<entry.first
                          << " packet count: " << entry.second << std::endl;
        }
       
        last_print_time = current_time;
    }
    
}
uint64_t UbSwitch::CalculatePacketQueueDelay(uint32_t packet_outPort){
      //  完整的数据包排队时延分析（包括 VOQ + Egress Queue）
         Ptr<Node> node = GetObject<Node>();
         Ptr<UbSwitch> ubSwitch = node->GetObject<UbSwitch>();
         Ptr<UbPort> outPortDevice = DynamicCast<UbPort>(node->GetDevice(packet_outPort));

         if (outPortDevice != nullptr && ubSwitch != nullptr) {
            // 1. Egress Queue 占用
            uint64_t egressBytes = outPortDevice->GetUbQueue()->GetCurrentBytes();
            
            // 2. VOQ 占用（该出端口所有入端口的队列总和）
            uint64_t voqBytes = ubSwitch->GetQueueManager()->GetTotalOutPortBufferUsed(packet_outPort);
            
            // 3. 总队列占用
            uint64_t totalQueueBytes = egressBytes + voqBytes;
            
            // 4. 端口速率
            DataRate portRate = outPortDevice->GetDataRate();
            
            // 5. 总排队时延
            double queueingDelayNanoSeconds = static_cast<double>((totalQueueBytes+PACKET_SIZE )* 8) / (portRate.GetBitRate()/1e9);
            Time queueingDelay = NanoSeconds(queueingDelayNanoSeconds);
            
            // 6. 详细输出
            if(false){
            std::cout << "[ACK 完整排队时延分析]" << std::endl;
            std::cout << "  - VOQ 队列长度：" << voqBytes << " bytes" << std::endl;
            std::cout << "  - Egress Queue 长度：" << egressBytes << " bytes" << std::endl;
            std::cout << "  - 总队列长度：" << totalQueueBytes << " bytes" << std::endl;
            std::cout << "  - 出端口发送速率：" << portRate.GetBitRate() / 1e9 << " Gbps" << std::endl;
            std::cout << "  - 预估总排队时延：" << queueingDelay.GetNanoSeconds() << " ns" << std::endl;
            }

            return queueingDelayNanoSeconds;
            
        }
        else{
            std::cout << "无法获取交换机对象或端口对象！" << std::endl;
            exit(0);
        }
}
/**
 * @brief Forward LDST data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers)
{
    // Get routing key from parsed headers
    RoutingKey rtKey;
    GetLdstRoutingKey(headers, rtKey);

    // Route
    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    if (outPort < 0) {
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        UbTransportChannel::s_totalSwitchRouteMissDrops++;
        return;
    }

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!3");
        UbTransportChannel::s_totalSwitchInPortDropsLdst++;
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

void UbSwitch::ForceShortestPathRouting(Ptr<Packet> packet, const UbDatalinkPacketHeader &parsedHeader)
{
    UbDatalinkPacketHeader modifiedHeader = parsedHeader;
    modifiedHeader.SetRoutingPolicy(true);  // Force shortest path

    UbDatalinkPacketHeader tempHeader;
    packet->RemoveHeader(tempHeader);
    packet->AddHeader(modifiedHeader);
}

void UbSwitch::ParseURMAPacketHeader(Ptr<Packet> packet, ParsedURMAHeaders &headers)
{
    // Parse headers needed by switch (store all that must be removed anyway)
    // Order: DLH -> NH -> IPv4 -> UDP -> TP -> ...
    packet->RemoveHeader(headers.datalinkPacketHeader);
    packet->RemoveHeader(headers.networkHeader);
    packet->RemoveHeader(headers.ipv4Header);
    packet->RemoveHeader(headers.udpHeader);
    packet->PeekHeader(headers.transportHeader);
    packet->AddHeader(headers.udpHeader);
    packet->AddHeader(headers.ipv4Header);
    packet->AddHeader(headers.networkHeader);
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::ParseLdstPacketHeader(Ptr<Packet> packet, ParsedLdstHeaders &headers)
{
    // Parse only headers needed by switch for routing and forwarding
    // Order: DLH -> CNA16NH -> dummyTA -> ...
    // Note: dummyTA can be either UbCompactTransactionHeader or UbCompactAckTransactionHeader
    packet->RemoveHeader(headers.datalinkPacketHeader);
    packet->RemoveHeader(headers.cna16NetworkHeader);
    packet->PeekHeader(headers.dummyTransactionHeader);
    packet->AddHeader(headers.cna16NetworkHeader);
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::GetURMARoutingKey(const ParsedURMAHeaders &headers, RoutingKey &rtKey)
{
    rtKey.sip = headers.ipv4Header.GetSource().Get();
    rtKey.dip = headers.ipv4Header.GetDestination().Get();
    rtKey.sport = headers.udpHeader.GetSourcePort();
    rtKey.dport = headers.udpHeader.GetDestinationPort();
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

void UbSwitch::GetLdstRoutingKey(const ParsedLdstHeaders &headers, RoutingKey &rtKey)
{
    uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
    uint16_t sCna = headers.cna16NetworkHeader.GetScna();
    uint32_t snode = utils::Cna16ToNodeId(sCna);
    uint32_t dnode = utils::Cna16ToNodeId(dCna);
    uint16_t sport = utils::Cna16ToPortId(sCna);
    uint16_t dport = 0;
    uint16_t lb = headers.cna16NetworkHeader.GetLb();
    rtKey.sip = utils::NodeIdToIp(snode, sport).Get();
    rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    rtKey.sport = lb;
    rtKey.dport = dport;
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

/**
 * @brief Packet enters VOQ from input port
 * Place packet into VOQ[outPort][priority][inPort] and update buffer statistics
 */
void UbSwitch::SendPacket(Ptr<Packet> packet, uint32_t inPort, uint32_t outPort, uint32_t priority)
{
    auto node = GetObject<Node>();
    Ptr<UbPort> recvPort = DynamicCast<ns3::UbPort>(node->GetDevice(inPort));

    m_voq[outPort][priority][inPort]->Push(packet);

    // Update both InPort and OutPort view buffer statistics
    m_queueManager->PushToVoq(inPort, outPort, priority, packet->GetSize());

    if (IsPFCEnable()) {
        recvPort->m_flowControl->HandleReceivedPacket(packet);
    }

    Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(outPort));
    port->TriggerTransmit();
}

/**
 * @brief Send control frame (PFC/CBFC) on specified port
 * Control frames use highest priority (0) and are locally generated (inPort = outPort)
 */
void UbSwitch::SendControlFrame(Ptr<Packet> packet, uint32_t portId)
{
    // Control frames: inPort = outPort, priority = 0
    uint32_t priority = 0;

    // Check if high priority buffer has space (should rarely be full)
    if (!m_queueManager->CheckInPortSpace(portId, priority, packet->GetSize())) {
        NS_LOG_WARN("High priority buffer full! Port=" << portId
                    << " This should rarely happen for control frames.");
    }

    SendPacket(packet, portId, portId, priority);
}

/**
 * @brief Packet dequeued from VOQ and moved to EgressQueue
 * Called by allocator when packet is selected from VOQ and placed into EgressQueue.
 * Updates buffer statistics to reflect packet leaving VOQ.
 */
void UbSwitch::NotifySwitchDequeue(uint16_t inPortId, uint32_t outPort, uint32_t priority, Ptr<Packet> packet)
{//std::cout << "NodeId:" << GetObject<Node>()->GetId() << " dequeues packet from VOQ: inPort:" << inPortId<< " outPort:" << outPort << " priority:" << priority << std::endl;
    // Update buffer statistics for all packets (including control frames)
    m_queueManager->PopFromVoq(inPortId, outPort, priority, packet->GetSize());

    // Only data packets trigger congestion control
    UbPacketType_t packetType = GetPacketType(packet);
    if (packetType != UB_CONTROL_FRAME) {
        NS_LOG_DEBUG("[QMU] Node:" << GetObject<Node>()->GetId()
              << " port:" << outPort
              << " VOQ outPort buffer:" << m_queueManager->GetTotalOutPortBufferUsed(outPort));
        m_congestionCtrl->SwitchForwardPacket(inPortId, outPort, packet);
    }
}

bool UbSwitch::IsCBFCEnable()
{
    return m_flowControlType == FcType::CBFC;
}

bool UbSwitch::IsCBFCSharedEnable()
{
    return m_flowControlType == FcType::CBFC_SHARED_CRD;
}

bool UbSwitch::IsPFCEnable()
{
    return m_flowControlType == FcType::PFC;
}

Ptr<UbQueueManager> UbSwitch::GetQueueManager()
{
    return m_queueManager;
}

void UbSwitch::SetCongestionCtrl(Ptr<UbCongestionControl> congestionCtrl)
{
    m_congestionCtrl = congestionCtrl;
}

Ptr<UbCongestionControl> UbSwitch::GetCongestionCtrl()
{
    return m_congestionCtrl;
}

void UbSwitch::LastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader)
{
    m_traceLastPacketTraversesNotify(nodeId, ubTpHeader);
}

}  // namespace ns3
