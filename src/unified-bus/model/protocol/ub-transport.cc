// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-caqm.h"
#include "../ub-network-address.h"
#include "ns3/node.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-utils.h"

using namespace utils;
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbTransportChannel");

NS_OBJECT_ENSURE_REGISTERED(UbTransportChannel);

uint64_t UbTransportChannel::s_totalDataPacketsSent = 0;
uint64_t UbTransportChannel::s_totalAcksReceived = 0;
uint64_t UbTransportChannel::s_totalRetransSent = 0;
uint64_t UbTransportChannel::s_totalpushretrans = 0;
bool UbTransportChannel::s_globalStatsReportScheduled = false;

TypeId UbTransportChannel::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransportChannel")
        .SetParent<UbIngressQueue>()
        .SetGroupName("UnifiedBus")
        .AddAttribute("EnableRetrans",
                      "Enable transport-layer retransmission.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::m_isRetransEnable),
                      MakeBooleanChecker())
        .AddAttribute("InitialRTO",
                      "Initial retransmission timeout in nanoseconds (RTO0).",
                      TimeValue(NanoSeconds(25600)),
                      MakeTimeAccessor(&UbTransportChannel::m_initialRto),
                      MakeTimeChecker())
        .AddAttribute("MaxRetransAttempts",
                      "Maximum retransmission attempts before aborting.",
                      UintegerValue(7),
                      MakeUintegerAccessor(&UbTransportChannel::m_maxRetransAttempts),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("RetransExponentFactor",
                      "Exponential factor of the number of retransmissions.",
                      UintegerValue(1),
                      MakeUintegerAccessor(&UbTransportChannel::m_retransExponentFactor),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("DefaultMaxWqeSegNum",
                      "Default limit on outstanding WQE segments per TP.",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&UbTransportChannel::m_defaultMaxWqeSegNum),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("DefaultMaxInflightPacketSize",
                      "Default cap on in-flight packets per TP.",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&UbTransportChannel::m_defaultMaxInflightPacketSize),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("TpOooThreshold",
                      "Receiver out-of-order PSN window size tracked in bitmap.",
                      UintegerValue(2048),
                      MakeUintegerAccessor(&UbTransportChannel::m_psnOooThreshold),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("UsePacketSpray",
                      "Enable per-packet ECMP/packet spray across multiple paths.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::m_usePacketSpray),
                      MakeBooleanChecker())
        .AddAttribute("UseShortestPaths",
                      "Sets a packet header flag that instructs switches to restrict forwarding to shortest paths (true) or allow non-shortest paths (false).",
                      BooleanValue(true),
                      MakeBooleanAccessor(&UbTransportChannel::m_useShortestPaths),
                      MakeBooleanChecker())
        .AddTraceSource("FirstPacketSendsNotify",
                        "Fires when the first packet of a WQE segment is sent.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceFirstPacketSendsNotify),
                        "ns3::UbTransportChannel::FirstPacketSendsNotify")
        .AddTraceSource("LastPacketSendsNotify",
                        "Fires when the last packet of a WQE segment is sent.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketSendsNotify),
                        "ns3::UbTransportChannel::LastPacketSendsNotify")
        .AddTraceSource("LastPacketACKsNotify",
                        "Fires when the last packet of a WQE segment is ACKed.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketACKsNotify),
                        "ns3::UbTransportChannel::LastPacketACKsNotify")
        .AddTraceSource("LastPacketReceivesNotify",
                        "Fires when the last packet of a WQE segment is received.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketReceivesNotify),
                        "ns3::UbTransportChannel::LastPacketReceivesNotify")
        .AddTraceSource("WqeSegmentSendsNotify",
                        "Fires when a WQE segment is scheduled for transmission.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceWqeSegmentSendsNotify),
                        "ns3::UbTransportChannel::WqeSegmentSendsNotify")
        .AddTraceSource("WqeSegmentCompletesNotify",
                        "Fires when a WQE segment completes at the receiver.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceWqeSegmentCompletesNotify),
                        "ns3::UbTransportChannel::WqeSegmentCompletesNotify")
        .AddTraceSource("TpRecvNotify",
                        "Fires on TP data or ACK reception (provides info and trace tags).",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_tpRecvNotify),
                        "ns3::UbTransportChannel::TpRecvNotify");
    return tid;
}

/**
 * @brief Constructor for UbTransportChannel
 */
UbTransportChannel::UbTransportChannel()
{
    BooleanValue val;
    if (GlobalValue::GetValueByNameFailSafe("UB_RECORD_PKT_TRACE", val)) {
        GlobalValue::GetValueByName("UB_RECORD_PKT_TRACE", val);
        m_pktTraceEnabled = val.Get();
    } else {
        m_pktTraceEnabled = false;
    }
    if (!s_globalStatsReportScheduled) {
        s_globalStatsReportScheduled = true;
        Simulator::ScheduleDestroy(&UbTransportChannel::ReportGlobalPacketStats);
    }
    NS_LOG_FUNCTION(this);
}

UbTransportChannel::~UbTransportChannel()
{
    // Clear WQE queues and release resources
    NS_LOG_INFO("tp release, node:" << m_src << " tpn:" << m_tpn);
    NS_LOG_FUNCTION(this);
}


void UbTransportChannel::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ackQ = queue<Ptr<Packet>>();
    m_wqeSegmentVector.clear();
    m_congestionCtrl = nullptr;
    m_recvPsnBitset.clear();
}

/**
 * @brief Get next packet from transport channel queue
 * Called by Switch Allocator during scheduling to retrieve the next packet for transmission
 */
Ptr<Packet> UbTransportChannel::GetNextPacket()
{

    auto routingProcess = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
    if (routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS ){
        return GetNextPacketForAlps();
    }
    // std::cout << "GetNextPacket called at time " << Simulator::Now().GetNanoSeconds() << " for TP " << m_tpn << std::endl;
    // 如果有ack，先发ack
    if (!m_ackQ.empty()) {
        Ptr<Packet> p = m_ackQ.front();
        m_ackQ.pop();
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }

    

    if (m_wqeSegmentVector.empty()) {
        NS_LOG_DEBUG("No WQE segments available to send");
        return nullptr;
    }

    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        return nullptr;
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];

        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }
        // 组数据包进行发送
        uint64_t payload_size = currentSegment->GetBytesLeft();
        if (payload_size > UB_MTU_BYTE) {
            payload_size = UB_MTU_BYTE;
        }

        // 计算剩余发送窗口，若不足以发送则返回nullptr。
        // caqm 算法使能时返回实际剩余窗口，未开启返回uint32MAX
        // 其余算法待拓展
        if (m_congestionCtrl->GetCongestionAlgo() == CAQM) {
            uint32_t rest = m_congestionCtrl->GetRestCwnd();
            if (rest < payload_size) {
                return nullptr;
            }
            NS_LOG_DEBUG("[Caqm send][restCwnd] Rest cwnd:" << rest);
        }

        Ptr<Packet> p = GenDataPacket(currentSegment, payload_size);
        ++s_totalDataPacketsSent;

        m_congestionCtrl->SenderUpdateCongestionCtrlData(m_psnSndNxt, payload_size);

        if (currentSegment->GetBytesLeft() == currentSegment->GetSize()) {
            // wqe segment first packet
            FirstPacketSendsNotify(m_nodeId, currentSegment->GetTaskId(), m_tpn, m_dstTpn,
                currentSegment->GetTpMsn(), m_psnSndNxt, m_sport);
        }
        if (currentSegment->GetBytesLeft() == payload_size) {
            // wqe segment last packet
            LastPacketSendsNotify(m_nodeId, currentSegment->GetTaskId(), m_tpn, m_dstTpn,
                currentSegment->GetTpMsn(), m_psnSndNxt, m_sport);
        }
        // PacketUid: TaskId: Tpn: Psn: PacketType: Src: Dst: PacketSize:
        NS_LOG_DEBUG("[Transport channel] Send packet."
                  << " PacketUid: " << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " DstTpn: " << m_dstTpn
                  << " Psn: " << m_psnSndNxt
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize()
                  << " TaskId: " << currentSegment->GetTaskId());
        currentSegment->UpdateSentBytes(payload_size);
        m_psnSndNxt++;
        // 发送时，更新定时器时间
        if (m_isRetransEnable) {
            if (m_retransEvent.IsExpired()) {
                // Schedules retransmit timeout. m_rto should be already doubled.
                m_rto = m_initialRto;
                NS_LOG_LOGIC(this << " SendDataPacket Schedule ReTxTimeout at time "
                                << Simulator::Now().GetNanoSeconds() << " to expire at time "
                                << (Simulator::Now().GetNanoSeconds() + m_rto.GetNanoSeconds()));
                m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
            }
        }
        // 属于本tp的这一轮wqe segment都发完了，继续向TA要
        if (m_psnSndNxt == m_tpPsnCnt) {
            ApplyNextWqeSegment();
        }
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }
    return nullptr;
}

Ptr<Packet> UbTransportChannel::GetNextPacketForAlps()
{
    // std::cout << "GetNextPacket called at time " << Simulator::Now().GetNanoSeconds() << " for TP " << m_tpn << std::endl;
    // 如果有ack，先发ack
    if (!m_ackQ.empty()) {
        Ptr<Packet> p = m_ackQ.front();
        m_ackQ.pop();
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }

    // ALPS: retransmission buffer has higher priority than new data packets.
    auto routingProcess = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
    if (routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS &&
        routingProcess->HasAlpsRetransPacket(m_tpn)) {
        PendingPkt retransPkt = routingProcess->PopAlpsRetransPacket(m_tpn);
       
        if (retransPkt.pktCopy) { 
            ++s_totalRetransSent;
            if(s_totalRetransSent%5000==0){
               std::cout<<"totalRetransSent:"<<s_totalRetransSent<<std::endl;
            }
            if(false){
                  std::cout<<"[ALPS RTX] Send retrans packet."
                         << " Tpn: " << retransPkt.srcTpn
                         << " Psn: " << retransPkt.psn
                         << " PacketUid: " << retransPkt.pktCopy->GetUid()<<std::endl;
            }
            AddAplsTagForRetransPacketOnHost(retransPkt.pktCopy, retransPkt.psn);
           
            return retransPkt.pktCopy->Copy();
        }
    }

    if (m_wqeSegmentVector.empty()) {
        NS_LOG_DEBUG("No WQE segments available to send");
        return nullptr;
    }

    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        return nullptr;
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];

        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }
        // 组数据包进行发送
        uint64_t payload_size = currentSegment->GetBytesLeft();
        if (payload_size > UB_MTU_BYTE) {
            payload_size = UB_MTU_BYTE;
        }

        // 计算剩余发送窗口，若不足以发送则返回nullptr。
        // caqm 算法使能时返回实际剩余窗口，未开启返回uint32MAX
        // 其余算法待拓展
        if (m_congestionCtrl->GetCongestionAlgo() == CAQM) {
            uint32_t rest = m_congestionCtrl->GetRestCwnd();
            if (rest < payload_size) {
                return nullptr;
            }
            NS_LOG_DEBUG("[Caqm send][restCwnd] Rest cwnd:" << rest);
        }

        Ptr<Packet> p = GenDataPacket(currentSegment, payload_size);
        ++s_totalDataPacketsSent;

        m_congestionCtrl->SenderUpdateCongestionCtrlData(m_psnSndNxt, payload_size);

        if (currentSegment->GetBytesLeft() == currentSegment->GetSize()) {
            // wqe segment first packet
            FirstPacketSendsNotify(m_nodeId, currentSegment->GetTaskId(), m_tpn, m_dstTpn,
                currentSegment->GetTpMsn(), m_psnSndNxt, m_sport);
        }
        if (currentSegment->GetBytesLeft() == payload_size) {
            // wqe segment last packet
            LastPacketSendsNotify(m_nodeId, currentSegment->GetTaskId(), m_tpn, m_dstTpn,
                currentSegment->GetTpMsn(), m_psnSndNxt, m_sport);
        }
        // PacketUid: TaskId: Tpn: Psn: PacketType: Src: Dst: PacketSize:
        NS_LOG_DEBUG("[Transport channel] Send packet."
                  << " PacketUid: " << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " DstTpn: " << m_dstTpn
                  << " Psn: " << m_psnSndNxt
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize()
                  << " TaskId: " << currentSegment->GetTaskId());
        currentSegment->UpdateSentBytes(payload_size);
        m_psnSndNxt++;
        // 发送时，更新定时器时间   ALPS不需要这块
        // if (m_isRetransEnable) {
        //     if (m_retransEvent.IsExpired()) {
        //         // Schedules retransmit timeout. m_rto should be already doubled.
        //         m_rto = m_initialRto;
        //         NS_LOG_LOGIC(this << " SendDataPacket Schedule ReTxTimeout at time "
        //                         << Simulator::Now().GetNanoSeconds() << " to expire at time "
        //                         << (Simulator::Now().GetNanoSeconds() + m_rto.GetNanoSeconds()));
        //         m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
        //     }
        // }
        // 属于本tp的这一轮wqe segment都发完了，继续向TA要
        if (m_psnSndNxt == m_tpPsnCnt) {
            ApplyNextWqeSegment();
        }
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }
    return nullptr;
}

uint32_t UbTransportChannel::GetNextPacketSize()
{
    uint32_t pktSize = 0;
    UbMAExtTah MAExtTaHeader;
    UbTransactionHeader TransactionHeader;
    UbTransportHeader  TransportHeader;
    UdpHeader   UHeader;
    Ipv4Header  I4Header;
    UbDatalinkPacketHeader  DataLinkPacketHeader;

    uint32_t MAExtTaHeaderSize = MAExtTaHeader.GetSerializedSize();
    uint32_t UbTransactionHeaderSize = TransactionHeader.GetSerializedSize();
    uint32_t UbTransportHeaderSize = TransportHeader.GetSerializedSize();
    uint32_t UdpHeaderSize = UHeader.GetSerializedSize();
    uint32_t Ipv4HeaderSize = I4Header.GetSerializedSize();
    uint32_t UbDataLinkPktSize = DataLinkPacketHeader.GetSerializedSize();

    uint32_t headerSize = MAExtTaHeaderSize + UbTransactionHeaderSize + UbTransportHeaderSize
                          + UdpHeaderSize + Ipv4HeaderSize + UbDataLinkPktSize;

    if (!m_ackQ.empty()) {
        return m_ackQ.front()->GetSize();
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }
        uint64_t payload_size = currentSegment->GetBytesLeft();
        if (payload_size > UB_MTU_BYTE) {
            payload_size = UB_MTU_BYTE;
        }
        pktSize = payload_size + headerSize;
        return pktSize;
    }
    return pktSize;
}

void UbTransportChannel::AddAplsTagForDatapacketOnHost(Ptr<Packet> p){
    //===============在host打tag。因为host只有一个端口，也可以考虑在此处进行路径选择====================
     auto node_type = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetNodeType();
     auto routing_algorithm = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm();
    
     //限定ALPS
    if(node_type!= UB_DEVICE||routing_algorithm != UbRoutingProcess::UbRoutingAlgorithm::ALPS) {
        return;//非host或非ALPS都不执行这个函数
    }

       
         Ipv4Address src = GetSip();
         Ipv4Address dst = GetDip();
         //std::cout<<"src:"<<src<<" dst:"<<dst<<std::endl;
        //从ip解析出节点ID
        uint32_t src_node_id = IpToNodeId(src);
        uint32_t dst_node_id = IpToNodeId(dst);

         //找到两节点的PST表
        uint64_t PstKey =UbRoutingProcess::HashPstKey(src_node_id, dst_node_id);
        
        auto rt = NodeList::GetNode(m_nodeId)->GetObject<ns3::UbSwitch>()->GetRoutingProcess();
        AlpsPstEntry* pstEntry = rt->GetPstEntry(PstKey);
        if (pstEntry == nullptr || pstEntry->PitEntries.empty()) {
         NS_LOG_ERROR("No valid PST entry found for ACK, reverse key:" << PstKey);
         std::cerr << "[ERROR] No valid PST entry for ACK! reversepstkey:" << PstKey << std::endl;
         return;
     }
         uint32_t path_id = rt->GetPidOnHostForPacketSpraying(pstEntry);//
         UbAlpsTag alpsTag;
         //初始化alpsTag
          //添加时间戳
        alpsTag.SetTimeStamp(Simulator::Now());
         alpsTag.SetPathId(path_id);
         alpsTag.SetHopCount(1);//=======================注意是从0开始还是1开始=========================
        p->AddPacketTag(alpsTag);
       // std::cout<<"node:"<<m_nodeId<<"为数据包打tag：path_id:"<<path_id<<"转发端口:"<<0<<std::endl;
         // ALPS sender-side buffering: keep one packet copy per (pid queue) for ACK matching/retrans.
       
         /**
         * @brief 将包复制一份，保存在ALPS sender-side buffering中，用于ACK匹配和重传
         * 
         */
        auto routingProcess = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
        //m_psnSndNxt就是数据包的psn
        PendingPkt pending(m_psnSndNxt, p->Copy(), m_tpn);
        routingProcess->RecordAlpsSentPacket(alpsTag.GetPathId(), pending);

}
void UbTransportChannel::AddAplsTagForRetransPacketOnHost(Ptr<Packet> p ,uint32_t psn){
    //===============在host打tag。因为host只有一个端口，也可以考虑在此处进行路径选择====================
     auto node_type = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetNodeType();
     auto routing_algorithm = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm();
    
     //限定ALPS
    if(node_type!= UB_DEVICE||routing_algorithm != UbRoutingProcess::UbRoutingAlgorithm::ALPS) {
        return;//非host或非ALPS都不执行这个函数
    }

       
         Ipv4Address src = GetSip();
         Ipv4Address dst = GetDip();
         //std::cout<<"src:"<<src<<" dst:"<<dst<<std::endl;
        //从ip解析出节点ID
        uint32_t src_node_id = IpToNodeId(src);
        uint32_t dst_node_id = IpToNodeId(dst);

         //找到两节点的PST表
        uint64_t PstKey =UbRoutingProcess::HashPstKey(src_node_id, dst_node_id);
        
        auto rt = NodeList::GetNode(m_nodeId)->GetObject<ns3::UbSwitch>()->GetRoutingProcess();
        AlpsPstEntry* pstEntry = rt->GetPstEntry(PstKey);
        if (pstEntry == nullptr || pstEntry->PitEntries.empty()) {
         NS_LOG_ERROR("No valid PST entry found for ACK, reverse key:" << PstKey);
         std::cerr << "[ERROR] No valid PST entry for ACK! reversepstkey:" << PstKey << std::endl;
         return;
     }
         uint32_t path_id = rt->GetPidOnHostForPacketSpraying(pstEntry);//
         UbAlpsTag alpsTag;
         //初始化alpsTag
          //添加时间戳
        alpsTag.SetTimeStamp(Simulator::Now());
         alpsTag.SetPathId(path_id);
         alpsTag.SetHopCount(1);//=======================注意是从0开始还是1开始=========================
        p->ReplacePacketTag(alpsTag);
       // std::cout<<"node:"<<m_nodeId<<"为数据包打tag：path_id:"<<path_id<<"转发端口:"<<0<<std::endl;
         // ALPS sender-side buffering: keep one packet copy per (pid queue) for ACK matching/retrans.
       
         /**
         * @brief 将包复制一份，保存在ALPS sender-side buffering中，用于ACK匹配和重传
         * 
         */
        auto routingProcess = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
        //
        PendingPkt pending(psn, p->Copy(), m_tpn);
        routingProcess->RecordAlpsSentPacket(alpsTag.GetPathId(), pending);

}

//从ALPS PST表中找到反向路径ID对应的路径，打tag，pathid是数据包传输路径，遍历反向pst的PIT集合然后找到反向路径pid与pathid对应的PIT
void UbTransportChannel::AddAplsTagForACKOnHost(Ptr<Packet> ackp, uint32_t pathid, uint32_t ackPsn){
    auto node_type = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetNodeType();
     auto routing_algorithm = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm();
    
     //限定ALPS
    if(node_type!= UB_DEVICE||routing_algorithm != UbRoutingProcess::UbRoutingAlgorithm::ALPS) {
        return;//非host或非ALPS都不执行这个函数
    }
    uint32_t reversepid=UbRoutingProcess::GetReservePid(pathid);
    UbAlpsTag alpsTag;
    //初始化alpsTag
              //加探测包的时候需要考虑这个字段
              //alpsTag.SetType(UbAlpsTag::TYPE_ACK);
              //添加时间戳
    alpsTag.SetTimeStamp(Simulator::Now());
    alpsTag.SetPathId(reversepid);
    alpsTag.SetHopCount(1);//=======================注意是从0开始还是1开始=========================
    alpsTag.SetAckPsn(ackPsn);
    ackp->AddPacketTag(alpsTag);
    //std::cout<<"node:"<<m_nodeId<<"为ACK打tag：reversepid:"<<reversepid<<"转发端口:"<<0<<std::endl;
    
}


Ptr<Packet> UbTransportChannel::GenDataPacket(Ptr<UbWqeSegment> wqeSegment, uint32_t payload_size)
{
    Ptr<Packet> p = Create<Packet>(payload_size);
    UbFlowTag flowTag(wqeSegment->GetTaskId(), wqeSegment->GetWqeSize());
    p->AddPacketTag(flowTag);
    // add UbMAExtTah
    UbMAExtTah MAExtTaHeader;
    MAExtTaHeader.SetLength(payload_size);
    p->AddHeader(MAExtTaHeader);
    // add TaHeader
    UbTransactionHeader TaHeader;
    TaHeader.SetTaOpcode(wqeSegment->GetType());
    TaHeader.SetIniTaSsn(wqeSegment->GetTaSsn());
    TaHeader.SetOrder(wqeSegment->GetOrderType());
    TaHeader.SetIniRcType(0x01);
    TaHeader.SetIniRcId(0xFFFFF);
    p->AddHeader(TaHeader);
    // add TpHeader
    UbTransportHeader TpHeader;
    if (wqeSegment->GetBytesLeft() == payload_size) {
        TpHeader.SetLastPacket(true); // last packet
    } else {
        TpHeader.SetLastPacket(false); // not last packet
    }
    TpHeader.SetTPOpcode(0x1);
    TpHeader.SetNLP(0x0);
    TpHeader.SetSrcTpn(m_tpn);
    TpHeader.SetDestTpn(m_dstTpn);
    TpHeader.SetAckRequest(1);
    TpHeader.SetErrorFlag(0);
    TpHeader.SetPsn(m_psnSndNxt);
    TpHeader.SetTpMsn(wqeSegment->GetTpMsn());
    p->AddHeader(TpHeader);
    // add udp header
    if (m_usePacketSpray) {
        if (m_lbHashSalt == UINT16_MAX) {
            m_lbHashSalt = 0;
        } else {
            m_lbHashSalt++;
        }
    }
    UbPort::AddUdpHeader(p, this);
    // add ipv4 header
    UbPort::AddIpv4Header(p, this);


    

   

    // add network header
    UbNetworkHeader networkHeader;
    if (m_congestionCtrl->GetCongestionAlgo() == CAQM) {
        networkHeader = m_congestionCtrl->SenderGenNetworkHeader();
    }
    p->AddHeader(networkHeader);
    // add dl header
    UbDataLink::GenPacketHeader(p, false, false, m_priority, m_priority, m_usePacketSpray,
                                m_useShortestPaths, UbDatalinkHeaderConfig::PACKET_IPV4);
    // =======在host打tag============
    AddAplsTagForDatapacketOnHost(p);
    return p;
}

/**
 * @brief Receive Transport Acknowledgment message
 * @param tpack Transport acknowledgment message to process
 * TP完成一个WQE后，产生TA ACK. 调用此函数将TA ACK传到TA
 */
void UbTransportChannel::RecvTpAck(Ptr<Packet> p)
{
    if (p == nullptr) {
        NS_LOG_ERROR("Null ack packet received");
        return;
    }
    UbAckTransactionHeader AckTaHeader;
    UbTransportHeader TpHeader;
    UbCongestionExtTph CETPH;
    p->RemoveHeader(TpHeader); // 处理接收包信息

    p->RemoveHeader(CETPH);

    
    if (TpHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)) {
        m_congestionCtrl->SenderRecvAck(TpHeader.GetPsn(), CETPH);
    }
    ++s_totalAcksReceived;
    p->RemoveHeader(AckTaHeader); // 处理接收包信息


    //解析APLStag
          //获取当前节点路由表
          auto RoutingProcess=NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
        if(RoutingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS){
           
          UbAlpsTag ACKalpsTag;
          p->PeekPacketTag( ACKalpsTag);
                    const uint32_t ackPsn = ACKalpsTag.GetAckPsn();
          uint64_t ACK_calDelay=Simulator::Now().GetNanoSeconds()-ACKalpsTag.GetTimeStamp().GetNanoSeconds();
                    NS_LOG_DEBUG("[ALPS ACK] node=" << m_nodeId
                                            << " tpn=" << m_tpn
                                            << " pid=" << ACKalpsTag.GetPathId()
                                            << " ackPsn=" << ackPsn);
          //std::cout<<"ACK带回来的时延:"<<ACK_calDelay<<" ns"<<std::endl;
           //获取ACK对应数据包的路径ID，进而获取路径的实时时延
          uint32_t packet_pid=UbRoutingProcess::GetReservePid(ACKalpsTag.GetPathId());
       // std::cout<<"此时路径真实时延:"<<RoutingProcess->GetPathRealDelay(packet_pid,m_src,m_dest)<<" ns"<<std::endl;
        //修改PIT表
         auto pstKey =RoutingProcess-> HashPstKey(m_src,m_dest);
        AlpsPstEntry* pstEntry = RoutingProcess->GetPstEntry( pstKey);
         for(auto &pitEntry:pstEntry->PitEntries){
            if(pitEntry->GetPathId()==packet_pid){
                //目前只更新了两个数据
                pitEntry->UpdateRealLatency(ACK_calDelay);
                pitEntry->UpdateLastUpdatedTime(Simulator::Now());
                break;
            }
            //std::cout<<"pitEntry->GetRealLatency():"<<pitEntry->GetRealLatency()<<std::endl;
         }
        }



    // 拿到多个packet后组成taack发送
    if ((TpHeader.GetPsn() + 1) > m_psnSndUna) {
        m_psnSndUna = TpHeader.GetPsn() + 1;
        if (m_sendWindowLimited && IsInflightLimited() == false) {
            m_sendWindowLimited = false;
            Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
            port->TriggerTransmit(); // 触发发送
        }
        NS_LOG_DEBUG("[Transport channel] Recv ack."
                  << " PacketUid: " << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnSndUna - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize());
        if (m_pktTraceEnabled) {
            UbFlowTag flowTag;
            p->PeekPacketTag(flowTag);
            UbPacketTraceTag traceTag;
            p->PeekPacketTag(traceTag);
            TpRecvNotify(p->GetUid(), m_psnSndUna - 1, m_dest, m_src, m_dstTpn, m_tpn,
                         PacketType::ACK, p->GetSize(), flowTag.GetFlowId(), traceTag);
        }

        // 收到有效ack后更新rto和超时重传次数为初始值，关闭超时事件并重新设定超时事件
        if (m_isRetransEnable) {
            m_rto = m_initialRto;
            m_retransAttemptsLeft = m_maxRetransAttempts;
            m_retransEvent.Cancel();
            NS_LOG_LOGIC(this << " Recv ack time " << Simulator::Now().GetNanoSeconds()
                            << " reset m_retransEvent at time "
                            << (Simulator::Now().GetNanoSeconds() + m_rto.GetNanoSeconds()));
            m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
        }
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size();) {
        if (m_psnSndUna >= (m_wqeSegmentVector[i]->GetPsnStart() + m_wqeSegmentVector[i]->GetPsnSize())) {
            // 对应ack的所有wqeSeg完成
            if (TpHeader.GetLastPacket()) {
                // 尾包ack被确认
                LastPacketACKsNotify(m_nodeId, m_wqeSegmentVector[i]->GetTaskId(), m_tpn, m_dstTpn,
                    TpHeader.GetTpMsn(), TpHeader.GetPsn(), m_sport);
            }
            auto ubTa = GetTransaction();
            if (ubTa->ProcessWqeSegmentComplete(m_wqeSegmentVector[i])) {
                WqeSegmentCompletesNotify(m_nodeId, m_wqeSegmentVector[i]->GetTaskId(), // 在这唯一地对应taskId了
                    m_wqeSegmentVector[i]->GetTaSsn());
                m_wqeSegmentVector.erase(m_wqeSegmentVector.begin() + i);
                // 当前vector中的segment数量小于2时申请调度Segment
                if (m_wqeSegmentVector.size() < 2) {
                    ApplyNextWqeSegment();
                }
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
    // tp从超过缓存限制的状态中恢复
    if (m_tpFullFlag && IsWqeSegmentLimited() == false) {
        m_tpFullFlag = false;
        ApplyNextWqeSegment();
    }
    if (m_isRetransEnable) {
        if (m_wqeSegmentVector.size() == 0) {
            m_retransEvent.Cancel(); // 如果确认流都完成，取消定时器
        }
    }
    if (m_congestionCtrl->GetCongestionAlgo() == CAQM && m_congestionCtrl->GetRestCwnd() >= UB_MTU_BYTE) {
        Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        port->TriggerTransmit(); // 触发发送
    }
    NS_LOG_DEBUG("Recv TP(data packet) acknowledgment");
}


/**
 * @brief Receive Transport Acknowledgment message
 * @param tpack Transport acknowledgment message to process
 * TP完成一个WQE后，产生TA ACK. 调用此函数将TA ACK传到TA
 */
void UbTransportChannel::RecvTpAckForAlps(Ptr<Packet> p)
{
    if (p == nullptr) {
        NS_LOG_ERROR("Null ack packet received");
        return;
    }
    UbAckTransactionHeader AckTaHeader;
    UbTransportHeader TpHeader;
    UbCongestionExtTph CETPH;
    p->RemoveHeader(TpHeader); // 处理接收包信息

    p->RemoveHeader(CETPH);

    //拥塞控制
    if (TpHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)) {
        m_congestionCtrl->SenderRecvAck(TpHeader.GetPsn(), CETPH);
    }
    ++s_totalAcksReceived;
    if(s_totalAcksReceived % 5000==0){
       std::cout<<"totalAcksReceived:"<<s_totalAcksReceived<<std::endl;
    }
    p->RemoveHeader(AckTaHeader); // 处理接收包信息


    //解析APLStag
          //获取当前节点路由表
          auto RoutingProcess=NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
        if(RoutingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS){
           
          UbAlpsTag ACKalpsTag;
          p->PeekPacketTag( ACKalpsTag);
                    const uint32_t ackPsn = ACKalpsTag.GetAckPsn();
          uint64_t ACK_calDelay=Simulator::Now().GetNanoSeconds()-ACKalpsTag.GetTimeStamp().GetNanoSeconds()+5+2;//5ns是host发送端发送一个4096B数据包的延迟
                    NS_LOG_DEBUG("[ALPS ACK] node=" << m_nodeId
                                            << " tpn=" << m_tpn
                                            << " pid=" << ACKalpsTag.GetPathId()
                                            << " ackPsn=" << ackPsn);
          //std::cout<<"ACK带回来的时延:"<<ACK_calDelay<<" ns"<<std::endl;
           //获取ACK对应数据包的路径ID，进而获取路径的实时时延
          uint32_t packet_pid=UbRoutingProcess::GetReservePid(ACKalpsTag.GetPathId());
       // std::cout<<"此时路径真实时延:"<<RoutingProcess->GetPathRealDelay(packet_pid,m_src,m_dest)<<" ns"<<std::endl;
        //修改PIT表
         auto pstKey =RoutingProcess-> HashPstKey(m_src,m_dest);
        AlpsPstEntry* pstEntry = RoutingProcess->GetPstEntry( pstKey);
         for(auto &pitEntry:pstEntry->PitEntries){
            if(pitEntry->GetPathId()==packet_pid){
                //目前只更新了两个数据
                //std::cout<<"pitEntry基准时延："<<pitEntry->GetBaseLatency()<<std::endl;
                pitEntry->UpdateRealLatency(ACK_calDelay);
                pitEntry->UpdateLastUpdatedTime(Simulator::Now());
                break;
            }
            //std::cout<<"pitEntry->GetRealLatency():"<<pitEntry->GetRealLatency()<<std::endl;
         }
            //都到这里来了似乎不用校验tpn了
            RoutingProcess->HandleAlpsAckByPsn(packet_pid, m_tpn, ackPsn,m_sport);
        }



    // 拿到多个packet后组成taack发送
    if ((TpHeader.GetPsn() + 1) > m_psnSndUna) {
        m_psnSndUna = TpHeader.GetPsn() + 1;
        if (m_sendWindowLimited && IsInflightLimited() == false) {
            m_sendWindowLimited = false;
            Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
            port->TriggerTransmit(); // 触发发送
        }
        NS_LOG_DEBUG("[Transport channel] Recv ack."
                  << " PacketUid: " << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnSndUna - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize());
        if (m_pktTraceEnabled) {
            UbFlowTag flowTag;
            p->PeekPacketTag(flowTag);
            UbPacketTraceTag traceTag;
            p->PeekPacketTag(traceTag);
            TpRecvNotify(p->GetUid(), m_psnSndUna - 1, m_dest, m_src, m_dstTpn, m_tpn,
                         PacketType::ACK, p->GetSize(), flowTag.GetFlowId(), traceTag);
        }
         //ALPS有自己的重传机制
        // // 收到有效ack后更新rto和超时重传次数为初始值，关闭超时事件并重新设定超时事件
        // if (m_isRetransEnable) {
        //     m_rto = m_initialRto;
        //     m_retransAttemptsLeft = m_maxRetransAttempts;
        //     m_retransEvent.Cancel();
        //     NS_LOG_LOGIC(this << " Recv ack time " << Simulator::Now().GetNanoSeconds()
        //                     << " reset m_retransEvent at time "
        //                     << (Simulator::Now().GetNanoSeconds() + m_rto.GetNanoSeconds()));
        //     m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
        // }
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size();) {
        if (m_psnSndUna >= (m_wqeSegmentVector[i]->GetPsnStart() + m_wqeSegmentVector[i]->GetPsnSize())) {
            // 对应ack的所有wqeSeg完成
            if (TpHeader.GetLastPacket()) {
                // 尾包ack被确认
                LastPacketACKsNotify(m_nodeId, m_wqeSegmentVector[i]->GetTaskId(), m_tpn, m_dstTpn,
                    TpHeader.GetTpMsn(), TpHeader.GetPsn(), m_sport);
            }
            auto ubTa = GetTransaction();
            if (ubTa->ProcessWqeSegmentComplete(m_wqeSegmentVector[i])) {
                WqeSegmentCompletesNotify(m_nodeId, m_wqeSegmentVector[i]->GetTaskId(), // 在这唯一地对应taskId了
                    m_wqeSegmentVector[i]->GetTaSsn());
                m_wqeSegmentVector.erase(m_wqeSegmentVector.begin() + i);
                // 当前vector中的segment数量小于2时申请调度Segment
                if (m_wqeSegmentVector.size() < 2) {
                    ApplyNextWqeSegment();
                }
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
    // tp从超过缓存限制的状态中恢复
    if (m_tpFullFlag && IsWqeSegmentLimited() == false) {
        m_tpFullFlag = false;
        ApplyNextWqeSegment();
    }
    // if (m_isRetransEnable) {
    //     if (m_wqeSegmentVector.size() == 0) {
    //         m_retransEvent.Cancel(); // 如果确认流都完成，取消定时器
    //     }
    // }
    // if (m_congestionCtrl->GetCongestionAlgo() == CAQM && m_congestionCtrl->GetRestCwnd() >= UB_MTU_BYTE) {
    //     Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    //     port->TriggerTransmit(); // 触发发送
    // }
    NS_LOG_DEBUG("Recv TP(data packet) acknowledgment");
}

void UbTransportChannel::ReportGlobalPacketStats()
{
    std::cout << "[UbTransportChannel] Global packet stats at simulation end:\n"
              << "  Total data packets sent: " << s_totalDataPacketsSent << "\n"
              << "  Total ACK packets received: " << s_totalAcksReceived << "\n"
              << "  Total HAVE retransmitted packets: " << s_totalRetransSent << "\n"
              << "  Total push retransmitted packets: " << s_totalpushretrans << std::endl;
}


void UbTransportChannel::SetUbTransport(uint32_t nodeId,
                                        uint32_t src,
                                        uint32_t dest,
                                        uint32_t srcTpn,        // TP Number
                                        uint32_t dstTpn,
                                        uint64_t size,          // Size parameter
                                        uint16_t priority,      // Process group identifier
                                        uint16_t sport,
                                        uint16_t dport,
                                        Ipv4Address sip,         // Source IP address
                                        Ipv4Address dip,         // Dest IP address
                                        Ptr<UbCongestionControl> congestionCtrl)
{
    m_nodeId = nodeId;
    m_src = src;
    m_dest = dest;
    m_tpn = srcTpn;
    m_dstTpn = dstTpn;
    m_size = size;
    m_priority = priority;
    m_sport = sport;
    m_dport = dport;
    m_sip = sip;
    m_dip = dip;
    m_congestionCtrl = congestionCtrl;
    m_congestionCtrl->TpInit(this);
    m_retransAttemptsLeft = m_maxRetransAttempts;
    m_maxQueueSize = m_defaultMaxWqeSegNum;
    m_maxInflightPacketSize = m_defaultMaxInflightPacketSize;
    m_recvPsnBitset.resize(m_psnOooThreshold, false);
}

/**
 * @brief Receive Data Packets
 * @param tpack Transport acknowledgment message to process
 * TP接收到一个数据包的时候，调用此函数处理，产生tpack
 */
void UbTransportChannel::RecvDataPacket(Ptr<Packet> p)
{
    if (p == nullptr) {
        NS_LOG_ERROR("Null packet received");
        return;
    }

    UbDatalinkPacketHeader pktHeader;
    UbTransactionHeader TaHeader;
    UbAckTransactionHeader AckTaHeader;
    UbTransportHeader TpHeader;
    UbCongestionExtTph CETPH;
    UbNetworkHeader NetworkHeader;
    UdpHeader udpHeader;
    Ipv4Header ipv4Header;
    UbMAExtTah MAExtTaHeader;
    Ptr<Packet> ackp = Create<Packet>(0); // ack不需要payloadsize
    p->RemoveHeader(pktHeader);
    p->RemoveHeader(NetworkHeader);
    p->RemoveHeader(ipv4Header);
    p->RemoveHeader(udpHeader);
    p->RemoveHeader(TpHeader);
    p->RemoveHeader(TaHeader); // 处理接收包信息
    p->RemoveHeader(MAExtTaHeader);
    //=======================为ACK打TAG===================
    
    if(NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS){
            UbAlpsTag DataPacketalpsTag;
            p->PeekPacketTag(DataPacketalpsTag);
            //std::cout<<"数据包经历时间:"<<Simulator::Now().GetNanoSeconds()-DataPacketalpsTag.GetTimeStamp().GetNanoSeconds()<<" ns"<<std::endl;
            uint32_t   DataPacketPid= DataPacketalpsTag.GetPathId();
            AddAplsTagForACKOnHost(ackp, DataPacketPid, TpHeader.GetPsn());
    }
    //==========================================
    uint64_t psn = TpHeader.GetPsn();
    NS_LOG_DEBUG("[Transport channel] Recv packet."
                  << " PacketUid: "  << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << psn
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize());
    UbFlowTag flowTag;
    p->PeekPacketTag(flowTag);
    if (m_pktTraceEnabled) {
        UbPacketTraceTag traceTag;
        p->PeekPacketTag(traceTag);
        TpRecvNotify(p->GetUid(), psn, m_dest, m_src, m_dstTpn, m_tpn,
                     PacketType::PACKET, p->GetSize(), flowTag.GetFlowId(), traceTag);
    }
    ackp->AddPacketTag(flowTag);
    if (TpHeader.GetLastPacket()) {
        // 尾包被接收
        LastPacketReceivesNotify(m_nodeId, TpHeader.GetSrcTpn(), TpHeader.GetDestTpn(), TpHeader.GetTpMsn(),
            TpHeader.GetPsn(), m_dport);
    }
    if (IsRepeatPacket(psn)) { // 收到重复包了，回复上一个ack
        TpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH); // 包类型变为ack
        /**
         * @brief 表示 ACK 确认的是“当前连续收到的最大 PSN”，不是“刚收到的这个包的 PSN”。
         * 
         */
        TpHeader.SetPsn(m_psnRecvNxt - 1);
        TpHeader.SetSrcTpn(m_tpn);
        TpHeader.SetDestTpn(m_dstTpn);
        CETPH.SetAckSequence(m_psnRecvNxt - 1);
        CETPH.SetLocation(NetworkHeader.GetLocation());
        CETPH.SetI(NetworkHeader.GetI());
        CETPH.SetC(NetworkHeader.GetC());
        CETPH.SetHint(NetworkHeader.GetHint());
        AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
        AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
        AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
        ackp->AddHeader(AckTaHeader);
        ackp->AddHeader(CETPH);
        ackp->AddHeader(TpHeader);
        ackp->AddHeader(udpHeader);
        UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
        ackp->AddHeader(NetworkHeader);
        UbDataLink::GenPacketHeader(ackp, false, true, pktHeader.GetCreditTargetVL(), pktHeader.GetPacketVL(),
            0, 1, UbDatalinkHeaderConfig::PACKET_IPV4);
        if (m_ackQ.empty()) {
            m_headArrivalTime = Simulator::Now();
        }
        m_ackQ.push(ackp); // 将ack放入队列
        NS_LOG_DEBUG("[Transport channel] Send ack. "
                  << " PacketUid: "  << ackp->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnRecvNxt - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ackp->GetSize());
        Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        port->TriggerTransmit(); // 触发发送
        return;
    }
    uint32_t psnStart = 0;
    uint32_t psnEnd = 0;
    if (psn >= m_psnRecvNxt) {
        // psn=m_psnRecvNxt代表顺序收到包，psn>m_psnRecvNxt代表乱序
        if (!SetBitmap(psn)) {
            // 超出bitmap允许的乱序规格了,先空着, 严重乱序，超过存储能力了，丢包不处理了(不回复ACK)，等重传
            NS_LOG_WARN("Over Out-of-Order! Max Out-of-Order :" << m_psnOooThreshold);
            return;
        }
        // 记录包号和size
        m_congestionCtrl->RecverRecordPacketData(psn, MAExtTaHeader.GetLength(), NetworkHeader);
         /**
         * @brief 乱序包不回复ack，只用记录了bitmap
         * 
         */
        // ALPS不区分乱序包和顺序包，都回复ack，其他算法乱序包不回复ack
        //if(NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm() != UbRoutingProcess::UbRoutingAlgorithm::ALPS){
          if (psn > m_psnRecvNxt) {
            NS_LOG_DEBUG("Out-of-Order Packet,tpn:{" << m_tpn << "} psn:{" << psn
                        << "} expectedPsn:{" << m_psnRecvNxt << "}");
            return; // 未开启sack的情况下乱序包不用回复ack，只用记录了bitmap
        }  
       // }
        
        uint32_t oldRecvNxt = m_psnRecvNxt;
        while (m_psnRecvNxt < oldRecvNxt + m_psnOooThreshold) {
            uint32_t currentBitIndex = m_psnRecvNxt - oldRecvNxt;
            if (currentBitIndex < m_recvPsnBitset.size() && m_recvPsnBitset[currentBitIndex]) {
                m_psnRecvNxt++;
            } else if (currentBitIndex) {
                break; // 遇到未确认的分段，停止
            }
        }
        // 如果 m_psnRecvNxt 有更新，需要清理 bitset
        if (m_psnRecvNxt > oldRecvNxt) {
            NS_LOG_DEBUG("Updated m_psnRecvNxt from " << oldRecvNxt
                        << " to " << m_psnRecvNxt);
            // 手动右移 bitset
            uint32_t shiftCount = m_psnRecvNxt - oldRecvNxt;
            RightShiftBitset(shiftCount);
            psnStart = oldRecvNxt;
            psnEnd = m_psnRecvNxt;
        }
    }
    NS_LOG_DEBUG("RecvDataPacket ready to send ack psn: " << (m_psnRecvNxt - 1) << " node: " << m_src);
    TpHeader.SetTPOpcode(m_congestionCtrl->GetTpAckOpcode());
    CETPH = m_congestionCtrl->RecverGenAckCeTphHeader(psnStart, psnEnd);
     /**
         * @brief 表示 ACK 确认的是“当前连续收到的最大 PSN”，不是“刚收到的这个包的 PSN”。
         * 
         */
    TpHeader.SetPsn(m_psnRecvNxt - 1);
    TpHeader.SetSrcTpn(m_tpn);
    TpHeader.SetDestTpn(m_dstTpn);
    AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
    AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
    ackp->AddHeader(AckTaHeader);
    ackp->AddHeader(CETPH);
    ackp->AddHeader(TpHeader);
    ackp->AddHeader(udpHeader);
    UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
    ackp->AddHeader(NetworkHeader);
    UbDataLink::GenPacketHeader(ackp, false, true, pktHeader.GetCreditTargetVL(), pktHeader.GetPacketVL(),
        0, 1, UbDatalinkHeaderConfig::PACKET_IPV4);
    if (m_ackQ.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    m_ackQ.push(ackp); // 将ack放入队列
    NS_LOG_DEBUG("[Transport channel] Send ack. "
                  << " PacketUid: "  << ackp->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnRecvNxt - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ackp->GetSize());
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}


void UbTransportChannel::RecvDataPacketForAlps(Ptr<Packet> p)
{
    if (p == nullptr) {
        NS_LOG_ERROR("Null packet received");
        return;
    }

    UbDatalinkPacketHeader pktHeader;
    UbTransactionHeader TaHeader;
    UbAckTransactionHeader AckTaHeader;
    UbTransportHeader TpHeader;
    UbCongestionExtTph CETPH;
    UbNetworkHeader NetworkHeader;
    UdpHeader udpHeader;
    Ipv4Header ipv4Header;
    UbMAExtTah MAExtTaHeader;
    Ptr<Packet> ackp = Create<Packet>(0); // ack不需要payloadsize
    p->RemoveHeader(pktHeader);
    p->RemoveHeader(NetworkHeader);
    p->RemoveHeader(ipv4Header);
    p->RemoveHeader(udpHeader);
    p->RemoveHeader(TpHeader);
    p->RemoveHeader(TaHeader); // 处理接收包信息
    p->RemoveHeader(MAExtTaHeader);
    //=======================为ACK打TAG===================
    
    if(NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS){
            UbAlpsTag DataPacketalpsTag;
            p->PeekPacketTag(DataPacketalpsTag);
            //std::cout<<"数据包经历时间:"<<Simulator::Now().GetNanoSeconds()-DataPacketalpsTag.GetTimeStamp().GetNanoSeconds()<<" ns"<<std::endl;
            uint32_t   DataPacketPid= DataPacketalpsTag.GetPathId();
            AddAplsTagForACKOnHost(ackp, DataPacketPid, TpHeader.GetPsn());
        //    if(p->GetUid()>=4294968016&&p->GetUid()<=4294968020|| TpHeader.GetPsn() <100){
        //     std::cout<<"Node"<<m_nodeId<<"ALPS 接收数据包: node=" << m_nodeId
        //             << " tpn=" << m_tpn
        //             << " psn=" << TpHeader.GetPsn() 
        //             << " PacketUid: " << p->GetUid()<<std::endl;
        //    }
            
    }
    //==========================================
    uint64_t psn = TpHeader.GetPsn();
    NS_LOG_DEBUG("[Transport channel] Recv packet."
                  << " PacketUid: "  << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << psn
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize());
    UbFlowTag flowTag;
    p->PeekPacketTag(flowTag);
    if (m_pktTraceEnabled) {
        UbPacketTraceTag traceTag;
        p->PeekPacketTag(traceTag);
        TpRecvNotify(p->GetUid(), psn, m_dest, m_src, m_dstTpn, m_tpn,
                     PacketType::PACKET, p->GetSize(), flowTag.GetFlowId(), traceTag);
    }
    ackp->AddPacketTag(flowTag);
    if (TpHeader.GetLastPacket()) {
        // 尾包被接收
        LastPacketReceivesNotify(m_nodeId, TpHeader.GetSrcTpn(), TpHeader.GetDestTpn(), TpHeader.GetTpMsn(),
            TpHeader.GetPsn(), m_dport);
    }
    if (IsRepeatPacket(psn)) { // 收到重复包了，回复上一个ack
        TpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH); // 包类型变为ack
        /**
         * @brief 表示 ACK 确认的是“当前连续收到的最大 PSN”，不是“刚收到的这个包的 PSN”。
         * 
         */
        TpHeader.SetPsn(m_psnRecvNxt - 1);
        TpHeader.SetSrcTpn(m_tpn);
        TpHeader.SetDestTpn(m_dstTpn);
        CETPH.SetAckSequence(m_psnRecvNxt - 1);
        CETPH.SetLocation(NetworkHeader.GetLocation());
        CETPH.SetI(NetworkHeader.GetI());
        CETPH.SetC(NetworkHeader.GetC());
        CETPH.SetHint(NetworkHeader.GetHint());
        AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
        AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
        AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
        ackp->AddHeader(AckTaHeader);
        ackp->AddHeader(CETPH);
        ackp->AddHeader(TpHeader);
        ackp->AddHeader(udpHeader);
        UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
        ackp->AddHeader(NetworkHeader);
        UbDataLink::GenPacketHeader(ackp, false, true, pktHeader.GetCreditTargetVL(), pktHeader.GetPacketVL(),
            0, 1, UbDatalinkHeaderConfig::PACKET_IPV4);
        if (m_ackQ.empty()) {
            m_headArrivalTime = Simulator::Now();
        }
        m_ackQ.push(ackp); // 将ack放入队列
        NS_LOG_DEBUG("[Transport channel] Send ack. "
                  << " PacketUid: "  << ackp->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnRecvNxt - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ackp->GetSize());
        Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        port->TriggerTransmit(); // 触发发送
        return;
    }
    uint32_t psnStart = 0;
    uint32_t psnEnd = 0;
    if (psn >= m_psnRecvNxt) {
        // psn=m_psnRecvNxt代表顺序收到包，psn>m_psnRecvNxt代表乱序
        if (!SetBitmap(psn)) {
            // 超出bitmap允许的乱序规格了,先空着, 严重乱序，超过存储能力了，丢包不处理了(不回复ACK)，等重传
            NS_LOG_WARN("Over Out-of-Order! Max Out-of-Order :" << m_psnOooThreshold);
            return;
        }
        // 记录包号和size
        m_congestionCtrl->RecverRecordPacketData(psn, MAExtTaHeader.GetLength(), NetworkHeader);
         /**
         * @brief 乱序包不回复ack，只用记录了bitmap
         * 
         */
        // ALPS不区分乱序包和顺序包，都回复ack，其他算法乱序包不回复ack
    if (psn > m_psnRecvNxt) {
        if(NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingAlgorithm() != UbRoutingProcess::UbRoutingAlgorithm::ALPS){
         
            NS_LOG_DEBUG("Out-of-Order Packet,tpn:{" << m_tpn << "} psn:{" << psn
                        << "} expectedPsn:{" << m_psnRecvNxt << "}");
            return; // 未开启sack的情况下乱序包不用回复ack，只用记录了bitmap
        }  
        else { // ALPS算法 发回乱序包ACK
                TpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH);
                TpHeader.SetPsn(m_psnRecvNxt - 1);
                TpHeader.SetSrcTpn(m_tpn);
                TpHeader.SetDestTpn(m_dstTpn);
                CETPH.SetAckSequence(m_psnRecvNxt - 1);
                CETPH.SetLocation(NetworkHeader.GetLocation());
                CETPH.SetI(NetworkHeader.GetI());
                CETPH.SetC(NetworkHeader.GetC());
                CETPH.SetHint(NetworkHeader.GetHint());
                AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
                AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
                AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
                ackp->AddHeader(AckTaHeader);
                ackp->AddHeader(CETPH);
                ackp->AddHeader(TpHeader);
                ackp->AddHeader(udpHeader);
                UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
                ackp->AddHeader(NetworkHeader);
                UbDataLink::GenPacketHeader(ackp,
                                            false,
                                            true,
                                            pktHeader.GetCreditTargetVL(),
                                            pktHeader.GetPacketVL(),
                                            0,
                                            1,
                                            UbDatalinkHeaderConfig::PACKET_IPV4);
                if (m_ackQ.empty()) {
                    m_headArrivalTime = Simulator::Now();
                }
                m_ackQ.push(ackp); // 将ack放入队列
                NS_LOG_DEBUG("[Transport channel] Send out-of-order cumulative ack. "
                             << " PacketUid: "  << ackp->GetUid()
                             << " Tpn: " << m_tpn
                             << " Psn: " << m_psnRecvNxt - 1
                             << " PacketType: Ack"
                             << " Src: " << m_src
                             << " Dst: " << m_dest
                             << " PacketSize: " << ackp->GetSize());
                Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
                port->TriggerTransmit(); // 触发发送
                return;
            }
        }
        
        uint32_t oldRecvNxt = m_psnRecvNxt;
        while (m_psnRecvNxt < oldRecvNxt + m_psnOooThreshold) {
            uint32_t currentBitIndex = m_psnRecvNxt - oldRecvNxt;
            if (currentBitIndex < m_recvPsnBitset.size() && m_recvPsnBitset[currentBitIndex]) {
                m_psnRecvNxt++;
            } else if (currentBitIndex) {
                break; // 遇到未确认的分段，停止
            }
        }
        // 如果 m_psnRecvNxt 有更新，需要清理 bitset
        if (m_psnRecvNxt > oldRecvNxt) {
            NS_LOG_DEBUG("Updated m_psnRecvNxt from " << oldRecvNxt
                        << " to " << m_psnRecvNxt);
            // 手动右移 bitset
            uint32_t shiftCount = m_psnRecvNxt - oldRecvNxt;
            RightShiftBitset(shiftCount);
            psnStart = oldRecvNxt;
            psnEnd = m_psnRecvNxt;
        }
    }
    NS_LOG_DEBUG("RecvDataPacket ready to send ack psn: " << (m_psnRecvNxt - 1) << " node: " << m_src);
    TpHeader.SetTPOpcode(m_congestionCtrl->GetTpAckOpcode());
    CETPH = m_congestionCtrl->RecverGenAckCeTphHeader(psnStart, psnEnd);
     /**
         * @brief 表示 ACK 确认的是“当前连续收到的最大 PSN”，不是“刚收到的这个包的 PSN”。
         * 
         */
    TpHeader.SetPsn(m_psnRecvNxt - 1);
    TpHeader.SetSrcTpn(m_tpn);
    TpHeader.SetDestTpn(m_dstTpn);
    AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
    AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
    ackp->AddHeader(AckTaHeader);
    ackp->AddHeader(CETPH);
    ackp->AddHeader(TpHeader);
    ackp->AddHeader(udpHeader);
    UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
    ackp->AddHeader(NetworkHeader);
    UbDataLink::GenPacketHeader(ackp, false, true, pktHeader.GetCreditTargetVL(), pktHeader.GetPacketVL(),
        0, 1, UbDatalinkHeaderConfig::PACKET_IPV4);
    if (m_ackQ.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    m_ackQ.push(ackp); // 将ack放入队列
    NS_LOG_DEBUG("[Transport channel] Send ack. "
                  << " PacketUid: "  << ackp->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnRecvNxt - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ackp->GetSize());
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}

void UbTransportChannel::ReTxTimeout()
{std::cout << "超时重传触发：ReTxTimeout" << std::endl;
    m_retransAttemptsLeft--;
    uint64_t rto = m_rto.GetNanoSeconds();
    rto = rto << m_retransExponentFactor; // 下一次超时重传变成Base_time * 2^(N*Times)
    m_rto = ns3::NanoSeconds(rto);
    NS_ASSERT_MSG (m_retransAttemptsLeft > 0, "Avaliable retransmission attempts exhausted.");
    // 重传逻辑
    m_psnSndNxt = m_psnSndUna; // 将发送指针回退到未确认的包
    // 重置已发送字节数
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        if (currentSegment->GetPsnStart() <= m_psnSndUna) {
            if (currentSegment->GetPsnStart() + currentSegment->GetPsnSize() > m_psnSndUna) {
                uint32_t  resetSentBytes =  (m_psnSndUna - currentSegment->GetPsnStart()) * UB_MTU_BYTE;
                currentSegment->ResetSentBytes(resetSentBytes); // 重置已发送字节数到未被确认的地方
                NS_LOG_INFO("Packet Retransmits,taskId: " << currentSegment->GetTaskId() << " psn: " << m_psnSndNxt);
            }
        } else {
            currentSegment->ResetSentBytes(); // 整个wqeSegment都未被确认，全重置已发送字节数
            NS_LOG_INFO("Packet Retransmits,taskId: " << currentSegment->GetTaskId() << " psn: " << m_psnSndNxt);
        }
    }

    // 重新发送
    m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}

/**
 * @brief Get current queue size
 * @return Current number of WQEs in queue
 */
uint32_t UbTransportChannel::GetCurrentSqSize() const
{
    return m_wqeSegmentVector.size();
}

bool UbTransportChannel::IsWqeSegmentLimited() const
{
    if (GetCurrentSqSize() >= m_maxQueueSize) {
        return true;
    }
    return false;
}

// 相当于发送窗口，应该与拥塞窗口取小值。目前尚未使用。
bool UbTransportChannel::IsInflightLimited() const
{
    if (m_psnSndNxt - m_psnSndUna >= m_maxInflightPacketSize) {
        return true;
    }
    return false;
}

/**
 * @brief Move right Bitset
 * @return
*/
void UbTransportChannel::RightShiftBitset(uint32_t shiftCount)
{
    if (shiftCount >= m_recvPsnBitset.size()) {
        std::fill(m_recvPsnBitset.begin(), m_recvPsnBitset.end(), false); // 清空所有位
        return;
    }

    // 手动实现右移
    for (size_t i = 0; i + shiftCount < m_recvPsnBitset.size(); ++i) {
        m_recvPsnBitset[i] = m_recvPsnBitset[i + shiftCount];
    }

    // 清空右移后的高位
    for (size_t i = m_recvPsnBitset.size() - shiftCount; i < m_recvPsnBitset.size(); ++i) {
        m_recvPsnBitset[i] = 0;
    }
}

/**
  * @brief Set bitmap
  * @return Set the PSN position to 1
*/
bool UbTransportChannel::SetBitmap(uint64_t psn)
{
    if (psn >= m_recvPsnBitset.size() + m_psnRecvNxt) {
        return false;
    }
    m_recvPsnBitset[psn - m_psnRecvNxt] = true;
    return true;
}

/**
  * @brief IsRepeatPacket
  * @return
*/
bool UbTransportChannel::IsRepeatPacket(uint64_t psn)
{
    if (psn < m_psnRecvNxt) {
        return true;
    }
    if (psn >= m_recvPsnBitset.size() + m_psnRecvNxt) {
        return false;
    }
    return m_recvPsnBitset[static_cast<int64_t>(psn) - static_cast<int64_t>(m_psnRecvNxt)];
}

void UbTransportChannel::WqeSegmentTriggerPortTransmit(Ptr<UbWqeSegment> segment)
{
    WqeSegmentSendsNotify(m_nodeId, segment->GetTaskId(), segment->GetTaSsn());
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}

Ptr<UbTransaction> UbTransportChannel::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

void UbTransportChannel::ApplyNextWqeSegment()
{
    GetTransaction()->ApplyScheduleWqeSegment(this);
}

bool UbTransportChannel::IsEmpty()
{
    if (!m_ackQ.empty()) {
        return false;
    }
    //新增ALPS重传缓冲区是否有数据包判定,有就返回非空
     auto routingProcess = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
    if (routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS ){
         if(routingProcess->HasAlpsRetransPacket(m_tpn)){
        return false;
       }
       if(m_nodeId == 0){
        //std::cout<<"Node"<<m_nodeId<<" alps retrans buffer is empty";
       }
       
    }
    if (m_wqeSegmentVector.empty()) {
        //std::cout<<"wqeSegmentVector is empty"<<std::endl;
        return true;
    }
    //std::cout<<"wqeSegmentVector is not empty"<<std::endl;
    return m_psnSndNxt >= m_tpPsnCnt;
}

bool UbTransportChannel::IsLimited()
{
    if (!m_ackQ.empty()) {
        return false;
    }
    //新增ALPS重传缓冲区是否有数据包判定,有就返回非空,因为重传包属于飞行中的包,所以窗口满了也可以触发重传
     auto routingProcess = NodeList::GetNode(m_nodeId)->GetObject<UbSwitch>()->GetRoutingProcess();
    if (routingProcess->GetRoutingAlgorithm() == UbRoutingProcess::UbRoutingAlgorithm::ALPS ){
         if(routingProcess->HasAlpsRetransPacket(m_tpn)){
        return false;
       }
       if(m_nodeId == 0){
        //std::cout<<"Node"<<m_nodeId<<" alps retrans buffer is empty"<<std::endl;
       }
       
    }

    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        return true;
    }
    if (m_congestionCtrl->GetCongestionAlgo() == CAQM) {
        if (m_congestionCtrl->GetRestCwnd() < UB_MTU_BYTE) {
            return true;
        }
    }
    return false;
}

IngressQueueType UbTransportChannel::GetIngressQueueType()
{
    return m_ingressQueueType;
}

void UbTransportChannel::FirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                                uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport)
{
    m_traceFirstPacketSendsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, mPsnSndNxt, mSport);
}

void UbTransportChannel::LastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                               uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport)
{
    m_traceLastPacketSendsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, mPsnSndNxt, mSport);
}

void UbTransportChannel::LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                              uint32_t mDstTpn, uint32_t tpMsn, uint32_t psn, uint32_t mSport)
{
    m_traceLastPacketACKsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, psn, mSport);
}

void UbTransportChannel::LastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                                  uint32_t tpMsn, uint32_t psn, uint32_t mDport)
{
    m_traceLastPacketReceivesNotify(nodeId, srcTpn, dstTpn, tpMsn, psn, mDport);
}

void UbTransportChannel::WqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    m_traceWqeSegmentSendsNotify(nodeId, taskId, taSsn);
}

void UbTransportChannel::WqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    m_traceWqeSegmentCompletesNotify(nodeId, taskId, taSsn);
}

void UbTransportChannel::TpRecvNotify(uint32_t packetUid, uint32_t psn, uint32_t src, uint32_t dst,
                                      uint32_t srcTpn, uint32_t dstTpn, PacketType type,
                                      uint32_t size, uint32_t taskId, UbPacketTraceTag traceTag)
{
    m_tpRecvNotify(packetUid, psn, src, dst, srcTpn, dstTpn, type, size, taskId, traceTag);
}

// ==========================================================================
// UbTransportGroup Implementation
// ==========================================================================

TypeId UbTransportGroup::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransportGroup")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbTransportGroup>();
    return tid;
}

UbTransportGroup::UbTransportGroup()
{
}

UbTransportGroup::~UbTransportGroup()
{
}

} // namespace ns3
