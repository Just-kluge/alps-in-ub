// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <limits>
#include "ns3/ub-controller.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"
#include "ns3/udp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include "random_generator.h"
#include "ns3/ub-alps.h"
using namespace utils;

#ifndef RTO_TIME
#define RTO_TIME 25600
#endif

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbRoutingProcess);
NS_LOG_COMPONENT_DEFINE("UbRoutingProcess");
std::unordered_map<uint32_t, uint32_t> UbRoutingProcess::m_Pid2ReservePid;
std::unordered_map<uint64_t, uint64_t> UbRoutingProcess::m_pstInitRateBps;
/*-----------------------------------------UbRoutingProcess----------------------------------------------*/
TypeId UbRoutingProcess::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRoutingProcess")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbRoutingProcess>()
        .AddAttribute("RoutingAlgorithm",
                    "Routing algorithm applied by UbRoutingProcess.",
                    EnumValue(UbRoutingAlgorithm::HASH),
                    MakeEnumAccessor<UbRoutingProcess::UbRoutingAlgorithm>(
                                    &UbRoutingProcess::m_routingAlgorithm),
                    MakeEnumChecker(UbRoutingAlgorithm::HASH, "HASH",
                                    UbRoutingAlgorithm::ADAPTIVE, "ADAPTIVE",
                                    UbRoutingAlgorithm::ALPS, "ALPS"));
    return tid;
}

UbRoutingProcess::UbRoutingProcess()
{
}

void UbRoutingProcess::AddShortestRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts)
{
    // 标准化端口集合（排序去重）
    std::vector<uint16_t> target;
    auto itRt = m_rtShortest.find(destIP);
    if (itRt != m_rtShortest.end()) {
        target.insert(target.end(), (*(itRt->second)).begin(), (*(itRt->second)).end());
    }
    target.insert(target.end(), outPorts.begin(), outPorts.end());
    std::vector<uint16_t> normalized = normalizePorts(target);
    
    // 查找或创建共享端口集合
    auto it = m_portSetPool.find(normalized);
    if (it != m_portSetPool.end()) {
        //  已存在相同端口集合，共享指针
        m_rtShortest[destIP] = it->second;
    } else {
        // 创建新端口集合并加入池中
        auto sharedPorts = std::make_shared<std::vector<uint16_t>>(normalized);
        m_portSetPool[normalized] = sharedPorts;
        m_rtShortest[destIP] = sharedPorts;
    }
}

void UbRoutingProcess::AddOtherRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts)
{
    // 标准化端口集合（排序去重）
    std::vector<uint16_t> target;
    auto itRt = m_rtOther.find(destIP);
    if (itRt != m_rtOther.end()) {
        target.insert(target.end(), (*(itRt->second)).begin(), (*(itRt->second)).end());
    }
    target.insert(target.end(), outPorts.begin(), outPorts.end());
    std::vector<uint16_t> normalized = normalizePorts(target);
    
    // 查找或创建共享端口集合
    auto it = m_portSetPool.find(normalized);
    if (it != m_portSetPool.end()) {
        // 已存在相同端口集合，共享指针
        m_rtOther[destIP] = it->second;
    } else {
        // 创建新端口集合并加入池中
        auto sharedPorts = std::make_shared<std::vector<uint16_t>>(normalized);
        m_portSetPool[normalized] = sharedPorts;
        m_rtOther[destIP] = sharedPorts;
    }
}

void UbRoutingProcess::AddAlpsRoute(const uint32_t pathID, const uint8_t outPort)
{
    AlpsRoutingEntry rtEntry(pathID, outPort);
    if (m_rtPathIdToOutPort.find(pathID) != m_rtPathIdToOutPort.end()) {
        NS_LOG_WARN("ALPS Route with pathID " << pathID << " already exists. It will be overwritten.");
    }
    m_rtPathIdToOutPort[pathID] = rtEntry;
}

void UbRoutingProcess::GetShortestOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts)
{
    outPorts.clear();
    auto it = m_rtShortest.find(destIP);
    if (it != m_rtShortest.end()) {
        outPorts.insert(outPorts.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
}

void UbRoutingProcess::GetOtherOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts)
{
    outPorts.clear();
    auto it = m_rtOther.find(destIP);
    if (it != m_rtOther.end()) {
        outPorts.insert(outPorts.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
}

void UbRoutingProcess::GetShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts)
{
    // 1. 首先基于目的节点的port地址进行选择
    GetShortestOutPorts(dip, outPorts);
    if (outPorts.empty()) {
        // 2. 如果找不到，掩盖port地址，使用主机的primary地址进行寻址
        Ipv4Mask mask("255.255.255.0");
        uint32_t maskedDip = Ipv4Address(dip).CombineMask(mask).Get();
        if (maskedDip != dip) {
            GetShortestOutPorts(maskedDip, outPorts);
            dip = maskedDip;
        }
    }

    // 3. 过滤掉入端口
    if (inPortId != UINT16_MAX) {
        auto it = std::remove_if(outPorts.begin(), outPorts.end(), 
                                  [inPortId](uint16_t port) { return port == inPortId; });
        outPorts.erase(it, outPorts.end());
    }
}

void UbRoutingProcess::GetNonShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts)
{
    // 1. 首先基于目的节点的port地址进行选择
    GetOtherOutPorts(dip, outPorts);
    if (outPorts.empty()) {
        // 2. 如果找不到，掩盖port地址，使用主机的primary地址进行寻址
        Ipv4Mask mask("255.255.255.0");
        uint32_t maskedDip = Ipv4Address(dip).CombineMask(mask).Get();
        if (maskedDip != dip) {
            GetOtherOutPorts(maskedDip, outPorts);
            dip = maskedDip;
        }
    }

    // 3. 过滤掉入端口
    if (inPortId != UINT16_MAX) {
        auto it = std::remove_if(outPorts.begin(), outPorts.end(), 
                                  [inPortId](uint16_t port) { return port == inPortId; });
        outPorts.erase(it, outPorts.end());
    }
}

const std::vector<uint16_t> UbRoutingProcess::GetAllOutPorts(const uint32_t destIP)
{
    std::vector<uint16_t> res;
    auto it = m_rtOther.find(destIP);
    if (it != m_rtOther.end()) {
        res.insert(res.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
    it = m_rtShortest.find(destIP);
    if (it != m_rtShortest.end()) {
        res.insert(res.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
    return res;
}


// 删除路由条目
bool UbRoutingProcess::RemoveShortestRoute(const uint32_t destIP)
{
    return m_rtShortest.erase(destIP) > 0;
}

// 删除路由条目
bool UbRoutingProcess::RemoveOtherRoute(const uint32_t destIP)
{
    return m_rtOther.erase(destIP) > 0;
}
void UbRoutingProcess::addPitEntryforPst(uint32_t src, uint32_t dst, AlpsPitEntry* pitEntry){
    uint64_t key = HashPstKey(src, dst);
    if(m_PstEntry.find(key) != m_PstEntry.end()) {
        m_PstEntry[key]->AddPitEntry(pitEntry);
    } else {
        AlpsPstEntry* pstEntry = new AlpsPstEntry(src, dst, 1, std::vector<AlpsPitEntry*>{pitEntry});
        m_PstEntry[key] = pstEntry;
    }
   
}
int UbRoutingProcess::SelectAdaptiveOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts,
                                             const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath)
{
    auto node = NodeList::GetNode(m_nodeId);
    auto ubSwitch = node->GetObject<UbSwitch>();
    auto queueManager = ubSwitch->GetQueueManager();
    uint8_t priority = rtKey.priority;

    auto calcLoadScore = [&](uint16_t outPort) -> uint64_t {
        if (queueManager == nullptr) {
            return 0;
        }
        // 使用OutPort视图统计VOQ占用
        uint64_t voqLoad = queueManager->GetOutPortBufferUsed(outPort, static_cast<uint32_t>(priority));
        
        // 加上EgressQueue的字节占用
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
        uint64_t egressLoad = port->GetUbQueue()->GetCurrentBytes();
        
        // 总负载 = VOQ + EgressQueue
        return voqLoad + egressLoad;
    };

    // 构造总候选列表：先 shortest，后 nonShortest
    std::vector<uint16_t> candidatePorts;
    candidatePorts.insert(candidatePorts.end(), shortestPorts.begin(), shortestPorts.end());
    candidatePorts.insert(candidatePorts.end(), nonShortestPorts.begin(), nonShortestPorts.end());

    if (candidatePorts.empty()) {
        return -1;
    }

    uint64_t bestScore = std::numeric_limits<uint64_t>::max();
    std::vector<uint16_t> bestPorts;
    size_t bestIndex = 0;
    for (size_t i = 0; i < candidatePorts.size(); ++i) {
        uint16_t port = candidatePorts[i];
        uint64_t score = calcLoadScore(port);
        if (score < bestScore) {
            bestScore = score;
            bestPorts.clear();
            bestPorts.push_back(port);
            bestIndex = i;
        } else if (score == bestScore) {
            bestPorts.push_back(port);
        }
    }

    if (bestPorts.empty()) {
        return -1;
    }

    // 通过索引判断是否选中最短路径
    selectedShortestPath = (bestIndex < shortestPorts.size());
    uint16_t selectedPort = bestPorts.front();
    return selectedPort;
}

uint64_t UbRoutingProcess::CalcHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint8_t priority)
{
    uint8_t buf[13];
    buf[0] = (sip >> 24) & 0xff;
    buf[1] = (sip >> 16) & 0xff;
    buf[2] = (sip >> 8) & 0xff;
    buf[3] = sip & 0xff;
    buf[4] = (dip >> 24) & 0xff;
    buf[5] = (dip >> 16) & 0xff;
    buf[6] = (dip >> 8) & 0xff;
    buf[7] = dip & 0xff;
    buf[8] = (sport >> 8) & 0xff;
    buf[9] = sport & 0xff;
    buf[10] = (dport >> 8) & 0xff;
    buf[11] = dport & 0xff;
    buf[12] = priority;
    std::string str(reinterpret_cast<const char*>(buf), sizeof(buf));
    uint64_t hash = Hash64(str);
    return hash;
}

int UbRoutingProcess::SelectOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts, 
                                     const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath)
{
    uint32_t sip = rtKey.sip;
    uint32_t dip = rtKey.dip;
    uint16_t sport = rtKey.sport;
    uint16_t dport = rtKey.dport;
    uint8_t priority = rtKey.priority;
    bool usePacketSpray = rtKey.usePacketSpray;

    size_t totalSize = shortestPorts.size() + nonShortestPorts.size();

    if (totalSize == 0) {
        return -1;
    }

    uint64_t hash64 = 0;
    if (usePacketSpray) {
        hash64 = CalcHash(sip, dip, sport, dport, priority);
    } else {
        // usePacketSpray == LB_MODE_PER_FLOW
        hash64 = CalcHash(sip, dip, 0, 0, priority);
    }
    
    size_t idx = hash64 % totalSize;
    
    // 通过索引判断是否选中最短路径，并直接返回对应集合中的端口
    if (idx < shortestPorts.size()) {
        selectedShortestPath = true;
        return shortestPorts[idx];
    } else {
        selectedShortestPath = false;
        return nonShortestPorts[idx - shortestPorts.size()];
    }
}

// 1. GetCandidatePorts基于useShortestPath选择可用的出端口集合
// 2. 基于用户设定的UbRoutingAlgorithm在candidatePorts中选择最终的出端口
// 2.1 如果是 HASH 算法，基于五元组哈希选择出端口(如果是usePacketSpray则使用完整五元组，否则掩盖sport和dport为0)
// 2.2 如果是 ADAPTIVE 算法，基于QueueManager信息选择负载最小的出端口
// 3. 如果找不到出端口，报错
int UbRoutingProcess::GetOutPort(RoutingKey &rtKey, bool &selectedShortestPath, uint16_t inPort)
{
    uint32_t sip = rtKey.sip;
    uint32_t dip = rtKey.dip;
    uint16_t sport = rtKey.sport;
    uint16_t dport = rtKey.dport;
    uint8_t priority = rtKey.priority;
    bool useShortestPath = rtKey.useShortestPath;
    bool usePacketSpray = rtKey.usePacketSpray;
    NS_LOG_DEBUG("[UbRoutingProcess GetOutPort]: sip: " << Ipv4Address(sip)
                << " dip: " << Ipv4Address(dip)
                << " sport: " << sport
                << " dport: " << dport
                << " priority: " << (uint16_t)priority
                << " useShortestPath: " << useShortestPath
                << " usePacketSpray: " << usePacketSpray);
    
    uint32_t tempDip = dip;
    
    // 分别获取最短路径和非最短路径候选端口
    std::vector<uint16_t> shortestPorts;
    std::vector<uint16_t> nonShortestPorts;
    GetShortestCandidates(tempDip, inPort, shortestPorts);
    if (!useShortestPath) {
        // 只有在不限制最短路径时，才获取非最短路径候选端口
        GetNonShortestCandidates(tempDip, inPort, nonShortestPorts);
    }
    
    // 检查是否有可用端口
    if (shortestPorts.empty() && nonShortestPorts.empty()) {
        NS_LOG_ERROR("No candidate ports found for dip: " << Ipv4Address(dip));
        return -1;
    }

    // 基于路由算法选择出端口，同时获得 selectedShortestPath 标记
    int outPortId = -1;
    if(m_routingAlgorithm == UbRoutingProcess::UbRoutingAlgorithm::HASH){
        outPortId = SelectOutPort(rtKey, shortestPorts, nonShortestPorts, selectedShortestPath);
    } else if(m_routingAlgorithm == UbRoutingProcess::UbRoutingAlgorithm::ADAPTIVE){
        outPortId = SelectAdaptiveOutPort(rtKey, shortestPorts, nonShortestPorts, selectedShortestPath);
    }

    // 若找不到出端口，报ASSERT
    NS_ASSERT_MSG(outPortId != -1, "No available output port found");
    
    return outPortId;
}

int UbRoutingProcess::GetOutPort(UbAlpsTag &alpsTag)
{
    uint32_t pathId = alpsTag.GetPathId();
    auto it = m_rtPathIdToOutPort.find(pathId);
    if (it != m_rtPathIdToOutPort.end()) {
        return it->second.outPort;
    } else {
        NS_LOG_ERROR("No output port found for ALPS path ID: " << pathId);
        return -1;
    }
}
uint32_t UbRoutingProcess::GetOutPort(uint32_t pathId)
{
    auto it = m_rtPathIdToOutPort.find(pathId);
    if (it != m_rtPathIdToOutPort.end()) {
        return it->second.outPort;
    } else {
        std::cout << "No output port found for ALPS path ID: " << pathId << std::endl;
        exit(1);
    }
}

void UbRoutingProcess::PrintAlpsRoute() const
{
    std::cout << "-------------------------------ALPS Routing Table for Node " << m_nodeId << " ---------------------------------------------------" << std::endl;
    for (const auto &entry : m_rtPathIdToOutPort)
    {
        uint32_t pathId = entry.first;
        uint8_t outPort = entry.second.outPort;
        std::cout << "Path ID: " << pathId << " -> Out Port: " << static_cast<uint16_t>(outPort) << std::endl;
    }
}


void UbRoutingProcess::RegisterPstInitialRateIfAbsent(uint32_t src, uint32_t dst, uint64_t initRateBps)
{
    if (initRateBps == 0) {
        return;
    }
    const uint64_t key = HashPstKey(src, dst);
    m_pstInitRateBps.emplace(key, initRateBps);
}


bool UbRoutingProcess::IsAllPerPathBdpFull(uint32_t src, uint32_t dst, uint32_t packetSize){
          auto pstKey = HashPstKey(src,dst);
        AlpsPstEntry* pstEntry = GetPstEntry( pstKey);
        for (const auto& entry : pstEntry->PitEntries) {
           if(!entry->IsPerPathBdpFull(packetSize))
           {
               return false;
           }
        }
        return true;
}






uint32_t UbRoutingProcess::GetPidOnHostForPacketSpraying(AlpsPstEntry* pstEntry, uint32_t packet_size){
    //std::cout<<"负载均衡"<<std::endl;
    uint32_t pitsize=pstEntry->PitEntries.size();     
    std::vector<double> weights;
    weights.resize(pitsize);
    uint64_t maxBaselatency=0;
    double sum_weights=0;
    //获取最大基准时延
   
    for(auto pitEntry : pstEntry->PitEntries){
        if(pitEntry->GetBaseLatency()>maxBaselatency){
            maxBaselatency=pitEntry->GetBaseLatency();
        }         
     }
    uint32_t i=0; 
    //std::cout<<"maxBaselatency:"<<maxBaselatency<<std::endl;
    //计算权重

    for (auto pitEntry : pstEntry->PitEntries)
     {
        // ALPS路径实时延迟按“无排队时延 + 端口映射里的最新排队时延”现算。
        const double realtimeLatency = static_cast<double>(pitEntry->GetRealTimeLatency(this)); // 这里GetRealTimeLatency会动态计算当前路径的实时时延，包含无排队时延和最新的排队时延。
        const double virtualLatency = pitEntry->GetVirtualLatencyNs();
        const double lbLatency = realtimeLatency + virtualLatency;
        double ratio = -1.0 * lbLatency / maxBaselatency;
        // 这里乘以初始权重=========4月11日修改-jyxiao
        if (pitEntry->IsPerPathBdpFull(0)) {
            weights[i] = 0.0;
            i++;
            continue;
        }
        weights[i] = std::exp(ratio)*pitEntry->GetWeight(); 
        sum_weights += weights[i]; 
        //std::cout<<"weights["<<i<<"]:"<<weights[i]<<std::endl;
        i++;
     }
     if (sum_weights == 0.0)
    {
       // 所有路径都被置零（例如 Per Path BDP 都满）时，发送侧 IsLimited 会阻止发包；这里保底返回首路径。
       if (!pstEntry->PitEntries.empty() && pstEntry->PitEntries.front() != nullptr) {
           return pstEntry->PitEntries.front()->GetPathId();
       }
       std::cout<<"sum_weights is zero"<<std::endl;
       exit(1);
    }
    //std::cout<<"数据包喷洒权重：:"<<sum_weights<<std::endl;
     //归一化
     for(uint32_t j=0;j<pitsize;j++){
        weights[j] /= sum_weights;
        //std::cout<<"weights["<<j<<"]:"<<weights[j]<<std::endl;
     }
     //根据权重选择路径
        // 使用 ns-3 的均匀分布随机变量，目前是替代方案，需要修改成我们的随机数生成代码
     // 使用 C++11 标准库生成随机数（性能更好）
    //static std::random_device rd;
    //static std::mt19937 generator(rd());
    //static std::uniform_real_distribution<double> distribution(0.0, 1.0);
    /**
     * @brief 已经更新，使用固定种子42的随机数生成器，返回一个0到1之间的随机double值，满足均匀分布，仿真结果可复现。
     * 
     */
    const double random_value = GenerateRandomDouble();
    // std::cout<<"random_value:"<<random_value<<std::endl<<std::endl;
     
    double cumulative_weight = 0.0;
    for (size_t k = 0; k < weights.size(); ++k) 
    {
        cumulative_weight += weights[k];
        if (random_value < cumulative_weight) 
        {
            pstEntry->PitEntries[k]->AddVirtualLatencyByPacketSize(packet_size);
            pstEntry->PitEntries[k]->UpdateLastUsedTime(Simulator::Now());
            pstEntry->PitEntries[k]->RecordSendPacket();
            pstEntry->PitEntries[k]->AddPerPathBdpInFlightBytes(packet_size);   
            return pstEntry->PitEntries[k]->GetPathId();
        }
    }
     pstEntry->PitEntries[weights.size()-1]->AddVirtualLatencyByPacketSize(packet_size);
     pstEntry->PitEntries[weights.size()-1]->UpdateLastUsedTime(Simulator::Now());
     pstEntry->PitEntries[weights.size()-1]->RecordSendPacket();
     pstEntry->PitEntries[weights.size()-1]->AddPerPathBdpInFlightBytes(packet_size); 
     return pstEntry->PitEntries[weights.size()-1]->GetPathId();         
}


 void UbRoutingProcess::PrintPid2ReservePid()  {
        std::cout << "=====PathID to ReversePathID Mapping:===================" << std::endl;
        for (const auto& entry : m_Pid2ReservePid) {
            std::cout << "PathID: " << entry.first << " -> ReversePathID: " << entry.second << std::endl;
        }
    }
    uint32_t UbRoutingProcess::GetReservePid(uint32_t pid){
             if(m_Pid2ReservePid.find(pid) != m_Pid2ReservePid.end()){
                return m_Pid2ReservePid[pid];
            } else {
                std::cout << "中断：No reserve PathID found for PathID: " << pid << std::endl;
                exit(1) ;
            }
    }
    uint64_t UbRoutingProcess::MakeNodePortKey(uint32_t nodeId, uint32_t outPort)
    {
        return (static_cast<uint64_t>(nodeId) << 32) | static_cast<uint64_t>(outPort);
    }

    void UbRoutingProcess::UpdateNodePortQueueDelay(uint32_t nodeId, uint32_t outPort, uint32_t delayNs)
    {
        m_NodePortQueueDelayNs[MakeNodePortKey(nodeId, outPort)] = delayNs;
    }

    uint32_t UbRoutingProcess::GetNodePortQueueDelay(uint32_t nodeId, uint32_t outPort)
    {
        auto it = m_NodePortQueueDelayNs.find(MakeNodePortKey(nodeId, outPort));
        if (it == m_NodePortQueueDelayNs.end()) {
            return 0;
        }
        return it->second;
    }
   uint64_t UbRoutingProcess::GetPathRealDelay(uint32_t packet_pid,uint32_t m_src,uint32_t m_dst){
        auto pstKey = HashPstKey(m_src,m_dst);
        AlpsPstEntry* pstEntry = GetPstEntry( pstKey);
        for (const auto& entry : pstEntry->PitEntries) {
            if (entry->GetPathId() == packet_pid) {
                 uint64_t realLatency = entry->GetNoQueueLatency();//（无排队时延）
                for(uint32_t i=1;i<entry->GetPorts().size();i++){
                    uint16_t outPort = entry->GetPorts()[i];
                    auto node = NodeList::GetNode(entry->GetNodes()[i]);
                    auto ubswitch = node->GetObject<UbSwitch>();
                    realLatency +=ubswitch->CalculatePacketQueueDelay(outPort); // 21ns是传输时延，CalculatePacketQueueDelay(outPort)是排队时延，这里假设每条链路的传输时延都是20ns，实际可以根据链路长度和速率计算得到。
                }


                
                return realLatency;
            }
        }
        std::cout << "中断：No real latency found for PathID: " << packet_pid << std::endl;
        exit(1);

   }

void UbRoutingProcess::RecordAlpsSentPacket(uint32_t pid, const PendingPkt& pkt,Ptr<UbHostAlps> alps)
{
    m_pendingByPid[pid].push_back(pkt);
    SetTimeoutForLapsbypid(pid, pkt.srcTpn, alps);
}


void UbRoutingProcess::HandleAlpsAckByPsn(uint32_t pid, uint32_t srcTpn, uint32_t ackPsn,uint32_t m_sport,Ptr<UbHostAlps> alps  )
{
    auto it = m_pendingByPid.find(pid);
    if (it == m_pendingByPid.end()) {
        return;
    }


    auto& pendingQ = it->second;
    bool matched = false;
    while (!pendingQ.empty()) {
        PendingPkt head = pendingQ.front();
        pendingQ.pop_front();
        //匹配成功就离开
        if ( head.psn == ackPsn&& head.srcTpn == srcTpn) {
            alps->AckBdpLikeInFlightBytes(head.pktCopy->GetSize());
            alps->AckPerPathBdpInFlightBytes(pid, head.pktCopy->GetSize());
            matched = true;
            break;  
        }
        m_retransBuffer[srcTpn].push_back(head);
        //丢失包去除记录
        alps->AckBdpLikeInFlightBytes(head.pktCopy->GetSize());
        alps->AckPerPathBdpInFlightBytes(pid, head.pktCopy->GetSize());
        std::cout <<"Node"<<m_nodeId<< " 记录了丢失的数据包,"<<"当前缓存数据包个数:"<<m_retransBuffer[srcTpn].size() << std::endl;

         ++UbTransportChannel::s_totalActiveRetransSent;
        if((UbTransportChannel::s_totalActiveRetransSent+UbTransportChannel::s_totaltimeoutretrans)%5000==0){
               std::cout<<"totalRetransSent:"<<UbTransportChannel::s_totalActiveRetransSent+UbTransportChannel::s_totaltimeoutretrans<<std::endl;
               std::cout<<"ActiveRetransSent:"<<UbTransportChannel::s_totalActiveRetransSent<<std::endl;
               std::cout<<"timeoutRetransSent:"<<UbTransportChannel::s_totaltimeoutretrans<<std::endl;
            }
    }
    if(!matched){
        std::cout<<"发送端接收的ACK清空了缓冲区还是没有找到匹配的数据包,属于误判丢包导致的大规模冗余重传"<<std::endl;
    }
    // 关键清理：该 pid 的待确认队列已空时，立即擦除键，避免 map 随运行时间单调膨胀。
    if (pendingQ.empty()) {
        m_pendingByPid.erase(it);
    }

    SetTimeoutForLapsbypid(pid,srcTpn,alps);

    // 关键修复
    if(!m_retransBuffer[srcTpn].empty()){
         Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        // Deleted:port->TriggerTransmit();
        Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
   }
    return;
}

bool UbRoutingProcess::HasAlpsRetransPacket(uint32_t tpn) const
{
    auto it = m_retransBuffer.find(tpn);
    return it != m_retransBuffer.end() && !it->second.empty();
}

PendingPkt UbRoutingProcess::PopAlpsRetransPacket(uint32_t tpn)
{    
    auto it = m_retransBuffer.find(tpn);
    if (it == m_retransBuffer.end() || it->second.empty()) {
        return PendingPkt();
    }

    auto& retransQ = it->second;
    PendingPkt pkt = retransQ.front();
    retransQ.pop_front();
    //std::cout <<"Node"<<m_nodeId<<"正在重传数据包,目前缓冲区待重传包个数:" << retransQ.size() << std::endl;
    if (retransQ.empty()) {
        m_retransBuffer.erase(it);
    }
    return pkt;
}

void UbRoutingProcess::SetTimeoutForLapsbypid(uint32_t pid, uint32_t srcTpn,Ptr<UbHostAlps> alps  ){
//在数据包发送以及ACK接收时触发
Time timeInNs = NanoSeconds(1000000);
auto it = m_rtoEventsPerPath.find(pid);

// 若该 pid 已无待确认数据，取消并清理定时器状态，避免事件表无界增长。
auto pendingIt = m_pendingByPid.find(pid);
if (pendingIt == m_pendingByPid.end() || pendingIt->second.empty()) {
    if (it != m_rtoEventsPerPath.end()) {
        if (it->second.IsPending()) {
            it->second.Cancel();
        }
        m_rtoEventsPerPath.erase(it);
    }
    return;
}

if (it != m_rtoEventsPerPath.end())
{
if (it->second.IsPending())
{NS_LOG_INFO("Cancel the timeout event that should be triggered at " << it->second.GetTs());
	it->second.Cancel();
}
//队列非空，重设超时计时器
EventId newEvent = Simulator::Schedule(timeInNs, &UbRoutingProcess::HandleTimeoutForLapsbypid, this, pid,srcTpn,alps);
m_rtoEventsPerPath[pid] = newEvent;
NS_LOG_INFO("Set a new timeout event for pid " << pid << " at " << Simulator::Now() + timeInNs);
        
}else

    {
	m_rtoEventsPerPath[pid] = Simulator::Schedule(timeInNs, &UbRoutingProcess::HandleTimeoutForLapsbypid, this, pid,srcTpn,alps);
	NS_LOG_INFO("Initial a new timeout event that should be triggered at " <<m_rtoEventsPerPath[pid].GetTs());

	}


}
void UbRoutingProcess::HandleTimeoutForLapsbypid(uint32_t pid, uint32_t srcTpn,Ptr<UbHostAlps> alps  ){
   std::cout<<"Node"<<m_nodeId<<" 超时事件触发"<<std::endl;
    auto it = m_pendingByPid.find(pid);
    if (it == m_pendingByPid.end()) {
        return;
    }


    auto& pendingQ = it->second;
      if(pendingQ.empty()){
        std::cout<<"Node"<<m_nodeId<<"超时事件触发时对应的待确认数据包队列已经空了，不需要重传"<<std::endl;
      }
    while (!pendingQ.empty()) {
        PendingPkt head = pendingQ.front();
        pendingQ.pop_front();
        m_retransBuffer[srcTpn].push_back(head);
        alps->AckBdpLikeInFlightBytes(head.pktCopy->GetSize());
        alps->AckPerPathBdpInFlightBytes(pid, head.pktCopy->GetSize());
        std::cout <<"Node"<<m_nodeId<< " 记录了丢失的数据包,"<<"当前缓存数据包个数:"<<m_retransBuffer[srcTpn].size() << std::endl;
        UbTransportChannel::s_totaltimeoutretrans++;
    }

    // 超时搬运完成后，清理空队列键与对应事件键，避免长期仿真期间容器残留。
    if (pendingQ.empty()) {
        m_pendingByPid.erase(it);
    }
    m_rtoEventsPerPath.erase(pid);
     Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(0));
     Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    
    }


} // namespace ns3
