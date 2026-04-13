// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <climits>
#include <limits>
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transport.h"
#include "../monitor/ub-monitor.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/data-rate.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/ub-port.h"
#include "ns3/ub-alps.h"
#include "ns3/ub-utils.h"
#include "ns3/global-value.h"

namespace ns3 {

static uint32_t GetRealHint(const uint32_t hint, const uint32_t ccUnit)
{
    // hint的值是mtu/ccunit的整数倍
    if (hint % (UB_MTU_BYTE / ccUnit) == 0) {
        return hint * ccUnit;
    } else { // 非整数倍
        uint32_t num = hint / (UB_MTU_BYTE / ccUnit);
        uint32_t realHint = num * UB_MTU_BYTE + (hint - num * (UB_MTU_BYTE / ccUnit));
        return realHint;
    }
}
static double
GetAlpsConfigDouble(const char* name, double defaultValue)
{
    DoubleValue value(defaultValue);
    if (GlobalValue::GetValueByNameFailSafe(name, value)) {
        return value.Get();
    }
    return defaultValue;
}

GlobalValue g_alpsSpeedupCooldownDivisor(
    "UB_ALPS_SPEEDUP_COOLDOWN_DIVISOR",
    "ALPS speedup cooldown divisor used by UbHostAlps::TrySpeedUpForALPS.",
    DoubleValue(8.0),
    MakeDoubleChecker<double>(1.0));

GlobalValue g_alpsSlowdownCooldownFactor(
    "UB_ALPS_SLOWDOWN_COOLDOWN_FACTOR",
    "ALPS slowdown cooldown divisor used by UbHostAlps::TrySlowDownForALPS.",
    DoubleValue(1.0),
    MakeDoubleChecker<double>(0.000001, 1000000.0));

GlobalValue g_alpsSlowdownRateDecayFactor(
    "UB_ALPS_SLOWDOWN_RATE_DECAY_FACTOR",
    "ALPS rate decay factor used by UbHostAlps::TrySlowDownForALPS.",
    DoubleValue(0.6),
    MakeDoubleChecker<double>(0.0, 1.0));

GlobalValue g_alpsInitialRateSameColFactor(
    "UB_ALPS_INITIAL_RATE_SAME_COL_FACTOR",
    "ALPS initial rate multiplier for same-col flows.",
    DoubleValue(0.7),
    MakeDoubleChecker<double>(0.0, 1.0));

GlobalValue g_alpsInitialRateOtherFactor(
    "UB_ALPS_INITIAL_RATE_OTHER_FACTOR",
    "ALPS initial rate multiplier for non-same-col flows.",
    DoubleValue(0.55),
    MakeDoubleChecker<double>(0.0, 1.0));

NS_LOG_COMPONENT_DEFINE("UbAlps");
NS_OBJECT_ENSURE_REGISTERED(UbAlps);

// Alps父类
TypeId UbAlps::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbAlps")
            .SetParent<ns3::UbCongestionControl>()
            .AddConstructor<UbAlps>()
            .AddAttribute("UbAlpsAlpha",
                          "α, alps window increase coefficient",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbAlps::m_alpha),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbAlpsBeta",
                          "β, alps window decrease coefficient",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbAlps::m_beta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbAlpsGamma",
                          "γ, window low limit coefficient",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbAlps::m_gamma),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbAlpsLambda",
                          "λ, switch cc calculate coefficient",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbAlps::m_lambda),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbAlpsTheta",
                          "θ, state reset time coefficient",
                          UintegerValue(10),
                          MakeUintegerAccessor(&UbAlps::m_theta),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UbAlpsQt",
                          "Qt, ideal max queue size in switch",
                          UintegerValue(10 * UB_MTU_BYTE),
                          MakeUintegerAccessor(&UbAlps::m_idealQueueSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UbAlpsCcUint",
                          "ccUnit, the number of bytes represented by one cc",
                          UintegerValue(32),
                          MakeUintegerAccessor(&UbAlps::m_ccUnit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UbMarkProbability",
                          "p, a packet marked probability",
                          DoubleValue(0.1),
                          MakeDoubleAccessor(&UbAlps::m_markProbability),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

UbAlps::UbAlps()
{
}

UbAlps::~UbAlps()
{
}

// host

static const double DATA_BYTE_RECVD_RESET_THREASHOLD = 0.9;
static const uint32_t DATA_BYTE_RECVD_RESET_NUM = 0x80000000; // 2 ^ 31

NS_OBJECT_ENSURE_REGISTERED(UbHostAlps);

TypeId UbHostAlps::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbHostAlps")
            .SetParent<ns3::UbAlps>()
            .AddConstructor<UbHostAlps>()
            .AddAttribute("UbAlpsCwnd",
                          "Initial congestion window",
                          UintegerValue(10 * UB_MTU_BYTE),
                          MakeUintegerAccessor(&UbHostAlps::m_cwnd),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

UbHostAlps::UbHostAlps()
{
    m_nodeType = UB_DEVICE;
}

UbHostAlps::~UbHostAlps()
{
    NS_LOG_FUNCTION(this);
}

void UbHostAlps::TpInit(Ptr<UbTransportChannel> tp)
{
    m_src = tp->GetSrc();
    m_dst = tp->GetDest();
    m_tpn = tp->GetTpn();

    Ptr<Node> senderNode = NodeList::GetNode(m_src);
    if (senderNode && senderNode->GetNDevices() > 0) {
        Ptr<UbPort> hostPort = DynamicCast<UbPort>(senderNode->GetDevice(0));
        if (hostPort) {
            m_maxRate = hostPort->GetDataRate();
        }
    }
    InitRateControlState();
}

// 获取剩余窗口，ALPS LDCP需要
uint32_t UbHostAlps::GetRestCwnd()
{
    if (m_congestionCtrlEnabled) {
        // 可能存在的特殊情况：拥塞状态下减窗，导致cwnd < inflight，则此情况返回0
        if (m_cwnd >= m_inFlight) {
            return m_cwnd - m_inFlight;
        } else {
            return 0;
        }
    } else {
        return UINT_MAX;
    }
}

// 发送端生成拥塞控制算法需要的header
UbNetworkHeader UbHostAlps::SenderGenNetworkHeader()
{
    NS_ASSERT_MSG(m_congestionCtrlEnabled && (m_algoType == CongestionCtrlAlgo::ALPS), "CC algorithm not ALPS or not enabled");
    UbNetworkHeader networkHeader;
    if (m_congestionCtrlEnabled) {
        networkHeader.SetI(0);
        networkHeader.SetC(0);
        networkHeader.SetHint(0);
    } else {
        networkHeader.SetI(0);
        networkHeader.SetC(0);
        networkHeader.SetHint(0);
    }
    return networkHeader;
}

// 发送端发包，更新数据
void UbHostAlps::SenderUpdateCongestionCtrlData(uint32_t psn, uint32_t size)
{
    if (m_congestionCtrlEnabled) {
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " Send pkt. Local:" << m_src
                      << " Send to:" << m_dst
                      << " Tpn:" << m_tpn
                      << " Psn:" << psn
                      << " Size:" << size
                      << " Send byte:" << m_dataByteSent
                      << " Inflight:" << m_inFlight);
    }
}

// 接收端接到数据包后记录数据
void UbHostAlps::RecverRecordPacketData(uint32_t psn, uint32_t size, UbNetworkHeader header)
{
    if (m_congestionCtrlEnabled) {

        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Local:" << m_src
                  << " recv from:" << m_dst
                  << " tpn:" << m_tpn
                  << " psn:" << psn
                  << " size:" << size
                  << " C:" << (int)header.GetC()
                  << " I:" << (int)header.GetI()
                  << " Hint:" << (int)header.GetHint());
    }
}

// 接收端生成拥塞控制算法需要的ack header
UbCongestionExtTph UbHostAlps::RecverGenAckCeTphHeader(uint32_t psnStart, uint32_t psnEnd)
{
    UbCongestionExtTph cetph;
    cetph.SetAckSequence(0);
        cetph.SetC(0);
        cetph.SetI(0);
        cetph.SetHint(0);
        return cetph;
}

// 发送端收到ack，调整窗口、速率等数据
void UbHostAlps::SenderRecvAck(uint32_t psn, UbCongestionExtTph header)
{
    if (!m_congestionCtrlEnabled) {
        return;
    }

    m_lastSequence = std::max(m_lastSequence, psn);

    Ptr<Node> senderNode = NodeList::GetNode(m_src);
    if (!senderNode) {
        std::cout << "Sender node is null" << std::endl;
        return;
    }
    Ptr<UbSwitch> senderSwitch = senderNode->GetObject<UbSwitch>();
    if (!senderSwitch) {
        std::cout << "Sender node has no switch" << std::endl;
        return;
    }

    Ptr<UbRoutingProcess> routingProcess = senderSwitch->GetRoutingProcess();
    if (!routingProcess) {
        std::cout << "Sender node has no routing process" << std::endl;
        return;
    }

    const uint64_t pstKey = UbRoutingProcess::HashPstKey(m_src, m_dst);
    AlpsPstEntry* pstEntry = routingProcess->GetPstEntry(pstKey);
    if (!pstEntry || pstEntry->PitEntries.empty()) {
        std::cout << "PST entry is null" << std::endl;
        return;
    }

    bool allPathsCongested = true;
    uint64_t maxBaseLatencyNs = 1;

    for (const auto* pit : pstEntry->PitEntries) {
        if (!pit) {
            std::cout << "PIT entry is null" << std::endl;
            continue;
        }
        const uint32_t baseLatency = std::max(1u, pit->GetBaseLatency());
        const uint64_t realLatency = pit->GetRealTimeLatency(routingProcess);
        maxBaseLatencyNs = std::max(maxBaseLatencyNs, static_cast<uint64_t>(baseLatency));
        if (realLatency <= baseLatency) {

            allPathsCongested = false;
        }
    }
    //std::cout << "maxBaseLatencyNs:" << maxBaseLatencyNs << std::endl;
    const Time maxBaseDelay = NanoSeconds(maxBaseLatencyNs);

    if (allPathsCongested) {
       TrySlowDownForALPS(maxBaseDelay);
    } else {
       TrySpeedUpForALPS(maxBaseDelay);
    }
          ///=================此处不对====================
    // if (changed) {
    //     m_nextSendTime = Simulator::Now();
    // }

    NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
              << "[Debug]"
              << "[" << __FUNCTION__ << "]"
              << " Local:" << m_src
              << " Recv from:" << m_dst
              << " Tpn:" << m_tpn
              << " Psn:" << psn
              << " AckSeq:" << header.GetAckSequence()
              << " C:" << static_cast<uint32_t>(header.GetC())
              << " I:" << static_cast<uint32_t>(header.GetI())
              << " Hint:" << header.GetHint()
              << " ConsecutiveSpeedups:" << m_consecutiveSpeedups
              << " Rate(bps):" << m_currentRate.GetBitRate());

}
void UbHostAlps::UpdateNextSendTime(uint32_t pktsize,uint32_t port){
     // 计算发送时延：(数据包大小 × 8 bits/byte) / 速率 (bps) = 时间 (秒)
    // 转换为纳秒：× 1e9
    double transmissionDelayNs = static_cast<double>(pktsize) * 8.0 * 1e9 / static_cast<double>(m_currentRate.GetBitRate());
    
    Time t = Simulator::Now() + NanoSeconds(transmissionDelayNs);
    
    //std::cout<<"NODE："<<m_src<<" UpdateNextSendTime: pkt size="<<pktsize<<" bytes, current rate="<<m_currentRate.GetBitRate()/1000000000<<" Gbps, "<<"NanoSeconds(transmissionDelayNs):"<<transmissionDelayNs<<"next send time="<<t.GetNanoSeconds()<<" ns"<<std::endl;    
     m_nextSendTime = t;
     // 关键修复：使用 Schedule 确保在下一个仿真时刻触发

     Ptr<UbPort> m_port = DynamicCast<UbPort>(NodeList::GetNode(m_src)->GetDevice(port));
     if(port!=0){
        std::cout<<"host应该只有编号为0的端口，但是此处端口为"<<port<<std::endl;
     }
     auto allocator = NodeList::GetNode(m_src)->GetObject<UbSwitch>()->GetAllocator();
      // 取消之前的定时器（如果有）
     if (m_nextSendTimerEvent.IsPending()) {
         Simulator::Cancel(m_nextSendTimerEvent);
     }
    // 调度下一次触发端口发送的定时器
     m_nextSendTimerEvent =Simulator::Schedule(NanoSeconds(transmissionDelayNs),&UbSwitchAllocator::TriggerAllocator, allocator, m_port);
      //std::cout<<"NODE："<<m_src<<" UpdateNextSendTime: pkt size="<<pktsize<<" bytes, current rate="<<m_currentRate.GetBitRate()/1000000000<<" Gbps, "<<"NanoSeconds(transmissionDelayNs):"<<transmissionDelayNs<<"next send time="<<t.GetNanoSeconds()<<" ns"<<std::endl;
    //Simulator::Schedule(NanoSeconds(transmissionDelayNs),&UbPort::TriggerTransmit, m_port);
}
void UbHostAlps::UpdateNextSendTimeForRateAdjustment(uint32_t RemaintransmissionDelayinNs){
    Time t = Simulator::Now() + NanoSeconds(RemaintransmissionDelayinNs);
    m_nextSendTime = t;
    //====================================host侧只有出端口，且编号为0，========================================
     Ptr<UbPort> m_port = DynamicCast<UbPort>(NodeList::GetNode(m_src)->GetDevice(0));
     auto allocator = NodeList::GetNode(m_src)->GetObject<UbSwitch>()->GetAllocator();
      // 取消之前的定时器（如果有）
     if (m_nextSendTimerEvent.IsPending()) {
         Simulator::Cancel(m_nextSendTimerEvent);
     }
    // 调度下一次触发端口发送的定时器
     m_nextSendTimerEvent =Simulator::Schedule(NanoSeconds(RemaintransmissionDelayinNs),&UbSwitchAllocator::TriggerAllocator, allocator, m_port);

    
}
Time UbHostAlps::GetNextSendTime(){
  return m_nextSendTime;
}
void UbHostAlps::InitRateControlState()
{
    Ptr<Node> senderNode = nullptr;
    if (m_src < NodeList::GetNNodes()) {
        senderNode = NodeList::GetNode(m_src);
    }

    if (senderNode) {
        auto ubSwitch = senderNode->GetObject<UbSwitch>();
        if (ubSwitch && ubSwitch->GetNodeType() == UB_SWITCH) {
            // 交换机不处理速率初始化，直接返回
            return;
        }
    }

    uint64_t maxRateBps = m_maxRate.GetBitRate();
    if (maxRateBps == 0) {
        maxRateBps = 400000000000ULL; // 400Gbps fallback
    }

   
    m_maxRate = DataRate(maxRateBps);
    m_currentRate = UbAlpsPacketTracker::EstimateInitialRateByType(m_src, m_dst, m_maxRate);
    m_baseRate = DataRate(std::min<uint64_t>(maxRateBps, m_currentRate.GetBitRate() * 2));
    //1Gbps的下限是为了避免过低的速率限制，实际使用中可以根据需要调整
    m_minRate = DataRate(std::min<uint64_t>(1000000000ULL, maxRateBps));
 //std::cout<<"NODE："<<m_src<<" InitRateControlState: maxRateBps="<<maxRateBps<<" bytes, flowCount="<<225<<" currentRate="<<m_currentRate.GetBitRate()/1000000000<<" Gbps"<<std::endl;
  
    m_nextSlowdownTime = Seconds(0);
    m_nextSpeedupTime = Seconds(0);
    m_nextSendTime = Seconds(0);
    m_consecutiveSpeedups = 0;
    // = true为允许动态调整发送速率
    m_rateLimitEnabled = true;
}

bool UbHostAlps::TrySpeedUpForALPS(Time maxBaseDelay)
{if(m_src==0&&m_dst==40){
        //std::cout << "TrySpeedUp" << std::endl;
    }
    if (!m_rateLimitEnabled || Simulator::Now() < m_nextSpeedupTime) {
        return false;
    }
    

    const uint64_t maxRateBps = std::max<uint64_t>(1, m_maxRate.GetBitRate());
    const uint64_t minRateBps = std::min<uint64_t>(m_minRate.GetBitRate(), maxRateBps);
    const uint64_t currentBps = std::max<uint64_t>(minRateBps, m_currentRate.GetBitRate());

    if (currentBps >= maxRateBps) {
        std::cout << "TrySpeedUp: Current rate is already at max" << std::endl;
        return false;
    }
        //计算当前数据包还有多少数据没发送
    uint64_t leftBits=(double)(currentBps)/1000000000*
    ((m_nextSendTime.GetNanoSeconds()-Simulator::Now().GetNanoSeconds())>=0
    ?m_nextSendTime.GetNanoSeconds()-Simulator::Now().GetNanoSeconds():0);

    uint64_t baseBps = std::max<uint64_t>(1, m_baseRate.GetBitRate());
    uint64_t newRateBps = currentBps;

    if (m_consecutiveSpeedups < 5) {
        newRateBps = (currentBps + baseBps) / 2;
        ++m_consecutiveSpeedups;
    } else {
        baseBps = std::min<uint64_t>(maxRateBps, currentBps * 2);
        m_baseRate = DataRate(baseBps);
        newRateBps = (currentBps + baseBps) / 2;
        m_consecutiveSpeedups = 1;
    }

    newRateBps = std::min<uint64_t>(maxRateBps, std::max<uint64_t>(minRateBps, newRateBps));
    m_currentRate = DataRate(newRateBps);
    // 根据新的发送速率和剩余窗口计算下次发送时间
    //std::cout<<"NODE："<<m_src<<" TrySpeedUp: "<<"current rate="<<currentBps/1000000000<<" Gbps, "<<"new rate="<<newRateBps/1000000000<<" Gbps, "<<"NanoSeconds(leftBits):"<<leftBits<<"ns"<<std::endl;
     uint32_t remainingTimeNs = leftBits * 1000000000 / newRateBps;
    UpdateNextSendTimeForRateAdjustment(remainingTimeNs);
    const Time cooldown = std::max(NanoSeconds(1), maxBaseDelay);
    const double cooldownDivisor = std::max(1.0, GetAlpsConfigDouble("UB_ALPS_SPEEDUP_COOLDOWN_DIVISOR", 8.0));
    m_nextSpeedupTime = Simulator::Now() + cooldown / cooldownDivisor;
    return true;
}

bool UbHostAlps::TrySlowDownForALPS(Time maxBaseDelay)
{   if(m_src==0&&m_dst==40){
        //std::cout << "TrySlowDown" << std::endl;
    }
    if (!m_rateLimitEnabled || Simulator::Now() < m_nextSlowdownTime) {
        return false;
    }
    
    const uint64_t maxRateBps = std::max<uint64_t>(1, m_maxRate.GetBitRate());
    const uint64_t minRateBps = std::min<uint64_t>(m_minRate.GetBitRate(), maxRateBps);
    const uint64_t currentBps = std::min<uint64_t>(maxRateBps, std::max<uint64_t>(minRateBps, m_currentRate.GetBitRate()));
    //更新当前速率以及期望速率
    m_baseRate = DataRate(currentBps);
    const double slowdownFactor = GetAlpsConfigDouble("UB_ALPS_SLOWDOWN_RATE_DECAY_FACTOR", 0.65);
    const uint64_t newRateBps = std::max<uint64_t>(minRateBps, static_cast<uint64_t>(currentBps * slowdownFactor));
    m_currentRate = DataRate(newRateBps);
    //计算当前数据包还有多少数据没发送
    uint64_t leftBits=(double)(currentBps)/1000000000*
    ((m_nextSendTime.GetNanoSeconds()-Simulator::Now().GetNanoSeconds())>=0
    ?m_nextSendTime.GetNanoSeconds()-Simulator::Now().GetNanoSeconds():0);
    
    m_consecutiveSpeedups = 0;
     // 根据新的发送速率和剩余窗口计算下次发送时间
    uint32_t remainingTimeNs = leftBits * 1000000000 / newRateBps;
    UpdateNextSendTimeForRateAdjustment(remainingTimeNs);
    //调整maxBaseDelay的时候m_nextSlowdownTime也会进行变动
    const Time cooldown = std::max(NanoSeconds(1), maxBaseDelay);
    const double cooldownFactor = std::max(0.000001, GetAlpsConfigDouble("UB_ALPS_SLOWDOWN_COOLDOWN_FACTOR",1));
    m_nextSlowdownTime = Simulator::Now() + cooldown / cooldownFactor;
    //m_nextSpeedupTime = Simulator::Now() + cooldown /4;
    return true;



}

void UbHostAlps::StateReset()
{
    m_congestionState = SLOW_START;
}

void UbHostAlps::DoDispose()
{
    NS_LOG_FUNCTION(this);
    Object::DoDispose();
}

// switch

NS_OBJECT_ENSURE_REGISTERED(UbSwitchAlps);

UbSwitchAlps::UbSwitchAlps()
{
    m_nodeType = UB_SWITCH;
    m_random = CreateObject<UniformRandomVariable>();
    m_random->SetAttribute("Min", DoubleValue(0.0));
    m_random->SetAttribute("Max", DoubleValue(1.0));
}

UbSwitchAlps::~UbSwitchAlps()
{
    NS_LOG_FUNCTION(this);
}

TypeId UbSwitchAlps::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbSwitchAlps")
            .SetParent<ns3::UbAlps>()
            .AddConstructor<UbSwitchAlps>()
            .AddAttribute("UbCcUpdatePeriod",
                          "Switch static cc update time",
                          TimeValue(NanoSeconds(500)),
                          MakeTimeAccessor(&UbSwitchAlps::m_ccUpdatePeriod),
                          MakeTimeChecker());

    return tid;
}

void UbSwitchAlps::SwitchInit(Ptr<UbSwitch> sw)
{
    auto node = sw->GetObject<Node>();
    m_nodeId = node->GetId();
    if (m_congestionCtrlEnabled) {
        uint32_t ndevice = node->GetNDevices();
        for (uint32_t i = 0; i < ndevice; i++) {
            m_txSize.push_back(0);
            m_cc.push_back(0);
            m_DC.push_back(0);
            m_creditAllocated.push_back(0);
            m_bps.push_back(DynamicCast<UbPort>(node->GetDevice(i))->GetDataRate());
        }
    }
    sw->SetCongestionCtrl(this); // 将拥塞控制算法设置到交换机中
}

void UbSwitchAlps::ResetLocalCc()
{
    if (m_congestionCtrlEnabled) {
        //空
        std::cout<<"ResetLocalCc, nodeId: "<<m_nodeId<<std::endl;
    }
}

void UbSwitchAlps::SetDataRate(uint32_t portId, DataRate bps)
{
    if (m_congestionCtrlEnabled) {
        m_bps[portId] = bps;
    }
}

void UbSwitchAlps::SwitchForwardPacket(uint32_t inPort, uint32_t outPort, Ptr<Packet> p)
{
    if (m_congestionCtrlEnabled) {
       // 空
    }
}

void UbSwitchAlps::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_cc.clear();
    m_txSize.clear();
    m_DC.clear();
    m_creditAllocated.clear();
    m_bps.clear();
    m_random = nullptr;
    Object::DoDispose();
}

}