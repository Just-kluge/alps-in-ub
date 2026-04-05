// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_ROUTING_PROCESS_H
#define UB_ROUTING_PROCESS_H

#include "ns3/node.h"
#include "ns3/ub-alps-tag.h"
#include "ns3/packet.h"
#include <set>
#include <deque>
#include <unordered_map>
#include "ns3/ub-alps-pst.h"
namespace ns3 {

class UbQueueManager;
class UbController;
class UbPacketQueue;

/**
 * @brief 查路由需要的相关参数
 */
struct RoutingKey {
    uint32_t sip;        // 源IP，或者SCNA
    uint32_t dip;        // 目的IP，或者DCNA
    uint16_t sport;      // UDP源端口，或者LB字段
    uint16_t dport;      // 目的端口，一般UDP为固定值4792；LDST CFG=9 一般拿不到这个值，写0
    uint8_t priority;    // 优先级
    bool useShortestPath;
    bool usePacketSpray;
};

struct AlpsRoutingEntry {
     uint32_t pathId;     // ALPS路径ID
     uint8_t outPort;     // 出端口
     AlpsRoutingEntry() : pathId(0), outPort(0) {}
     AlpsRoutingEntry(uint32_t id, uint8_t port) : pathId(id), outPort(port) {}
};

struct PendingPkt {
    uint32_t psn;
    Ptr<Packet> pktCopy;
    uint32_t srcTpn;

    PendingPkt() : psn(0), pktCopy(nullptr), srcTpn(0) {}
    PendingPkt(uint32_t p, Ptr<Packet> pkt, uint32_t tpn) : psn(p), pktCopy(pkt), srcTpn(tpn) {}
};

/**
 * @brief 路由模块
 */
class UbRoutingProcess : public Object {
public:

    // 操作类型枚举
    enum class UbRoutingAlgorithm : uint8_t {
        HASH = 0,   // Hash-based routing
        ADAPTIVE = 1,   // Adaptive routing
        ALPS = 2   // ALPS routing
    };

    UbRoutingProcess();
    ~UbRoutingProcess() {}
    static TypeId GetTypeId(void);
    void SetNodeId(uint32_t nodeId) {m_nodeId = nodeId;}

    //获取路由策略名称
    UbRoutingAlgorithm GetRoutingAlgorithm() const { return m_routingAlgorithm; }
    // 添加路由条目
    void AddShortestRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts);
    void AddOtherRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts);
    void AddAlpsRoute(const uint32_t pathID, const uint8_t outPort);
    
    // 打印路由条目
    void PrintAlpsRoute() const;

    void GetShortestOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts);
    void GetOtherOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts);
    const std::vector<uint16_t> GetAllOutPorts(const uint32_t destIP);
    
    // 获取最短路径候选端口和非最短路径候选端口
    void GetShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts);
    void GetNonShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts);
    int GetOutPort(RoutingKey &rtKey, bool &selectedShortestPath, uint16_t inPort = UINT16_MAX);
    int GetOutPort(UbAlpsTag &alpsTag); // ALPS专用接口，根据ALPSTag中的路径ID选择出端口
    uint32_t GetOutPort(uint32_t pathId); // ALPS专用接口，根据路径ID选择出端口
    int SelectOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts, 
                      const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath);
    // 自适应路由策略
    int SelectAdaptiveOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts,
                              const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath);
    // 删除路由条目
    bool RemoveShortestRoute(const uint32_t destIP);
    bool RemoveOtherRoute(const uint32_t destIP);
//===========================================================================================
    void addPitEntryforPst(uint32_t src, uint32_t dst, AlpsPitEntry* pitEntry);
    static uint64_t HashPstKey(uint32_t src, uint32_t dst) {
        std::string key = std::to_string(src) + "-" + std::to_string(dst);
        return Hash64(key);
    }
    void PrintPstEntry(uint32_t src, uint32_t dst) const {
        uint64_t key = HashPstKey(src, dst);
        auto it = m_PstEntry.find(key);
        if (it != m_PstEntry.end()) {
            std::cout << "PST Entry for src: " << src << ", dst: " << dst << " -> " << it->second->ToString() << std::endl;
        } else {
            std::cout << "No PST Entry found for src: " << src << ", dst: " << dst << std::endl;
        }
    }
    /**
     * @brief 用节点对的hash值作为key从PST表中获取对应的PST条目
     * 
     * @param PstKey 
     * @return AlpsPstEntry* 
     */
      AlpsPstEntry* GetPstEntry( uint64_t PstKey){
        if(m_PstEntry.find(PstKey) != m_PstEntry.end()) {
            return m_PstEntry[PstKey];
        }
        std::cout << "No PST Entry found for PstKey: " << PstKey << ", Node: " << m_nodeId<< std::endl;
        exit(1);
      }
      /**
       * @brief 在发送端根据候选路径时延构建路径权重，再根据权重选择路径
       * 
       * @param pstEntry 
       * @return uint32_t 
       */
    uint32_t GetPidOnHostForPacketSpraying( AlpsPstEntry* pstEntry);
    /**
     * @brief 在收到ACK时获取路径此时的瞬时时延，然后与ACK测量的结果进行比较（输出到控制台或者进行误差统计）
     * 输入：路径ID，源节点id，目的节点id
     * 输出：路径的瞬时时延
     */
     uint64_t GetPathRealDelay(uint32_t packet_pid,uint32_t m_src,uint32_t m_dst);
     /**
     * @brief 打印路径id与反向路径id映射
     */
    static void PrintPid2ReservePid() ;
    /**
     * @brief 根据路径ID获取反向路径ID(构建ACK时，基于数据包路径ID推导其反向路径ID。；用于ACK利用自身PID在中间交换机解析数据包路径PID，进而定位出端口。)
     * 输入：路径ID
     * 输出：反向路径ID
     */
    static uint32_t GetReservePid(uint32_t pid);
    static uint64_t MakeNodePortKey(uint32_t nodeId, uint32_t outPort);
     void UpdateNodePortQueueDelay(uint32_t nodeId, uint32_t outPort, uint32_t delayNs);
     uint32_t GetNodePortQueueDelay(uint32_t nodeId, uint32_t outPort);
    /**
     * @brief 构建路径id与反向路径id映射,根据某一路径pid查找反向路径pid，ACK用反向路径pid在交换机上可以快速找到其数据包的出端口。
     */
    static std::unordered_map<uint32_t, uint32_t> m_Pid2ReservePid; // 
    // key=(nodeId<<32)|outPort，value=该端口最近一次观测到的数据包排队时延(ns)
     std::unordered_map<uint64_t, uint32_t> m_NodePortQueueDelayNs;

    // ALPS sender-side packet buffer APIs (step 4: declarations only).
    void RecordAlpsSentPacket(uint32_t pid, const PendingPkt& pkt);
    void HandleAlpsAckByPsn(uint32_t pid, uint32_t srcTpn, uint32_t ackPsn,uint32_t m_sport);
    void SetTimeoutForLapsbypid(uint32_t pid, uint32_t srcTpn);
    void HandleTimeoutForLapsbypid(uint32_t pid, uint32_t srcTpn);
    bool HasAlpsRetransPacket(uint32_t tpn) const;
    PendingPkt PopAlpsRetransPacket(uint32_t tpn);
//============================================================================
private:
    struct VectorHash {
        size_t operator()(const std::vector<uint16_t>& v) const
        {
            std::string hashKey;
            for (int i : v) {
                hashKey += std::to_string(i);
            }
            return Hash64(hashKey);
        }
    };

    uint32_t m_nodeId;
    UbRoutingAlgorithm m_routingAlgorithm = UbRoutingAlgorithm::HASH;
    //UbRoutingAlgorithm m_routingAlgorithm = UbRoutingAlgorithm::ALPS;
    uint64_t CalcHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint8_t priority);

    // 全局端口集合池：存储所有唯一的端口集合
    std::unordered_map<std::vector<uint16_t>, std::shared_ptr<std::vector<uint16_t> >, VectorHash> m_portSetPool;
    
    // 路由表：目的IP -> 共享的端口集合指针
    std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint16_t> > > m_rtShortest;
    std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint16_t> > > m_rtOther;

    std::unordered_map<uint32_t, AlpsRoutingEntry> m_rtPathIdToOutPort; // 路径ID到出端口的映射表，供ALPS使用
    std::unordered_map<uint64_t, AlpsPstEntry*> m_PstEntry;// ALPS PST表，key是src-dst的hash值

    // ALPS sender-side buffers keyed by pid.
    // pid -> deque of pending packets
    std::unordered_map<uint32_t, std::deque<PendingPkt>> m_pendingByPid;
    //tpn->deque<PendingPkt>
    std::unordered_map<uint32_t, std::deque<PendingPkt>> m_retransBuffer;
    std::unordered_map<uint32_t, EventId> m_rtoEventsPerPath;
    
    // 辅助函数：标准化端口集合（排序去重）
    std::vector<uint16_t> normalizePorts(const std::vector<uint16_t>& ports)
    {
        std::set<uint16_t> sorted(ports.begin(), ports.end());
        return std::vector<uint16_t>(sorted.begin(), sorted.end());
    }
};

} // namespace ns3

#endif /* UB_RT_TABLE_H */
