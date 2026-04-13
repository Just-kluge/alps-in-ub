#ifndef UB_ALPS_PST_H
#define UB_ALPS_PST_H
#include <unordered_map>
#include <vector>
#include <time.h>
#include <random>
#include <sstream>      
#include <iomanip>
#include <string>
#include <ostream>
#include "ns3/ptr.h"
#include <ns3/nstime.h>
#ifndef DEFAULT_PRECISION_FOR_FLOAT_TO_STRING
#define DEFAULT_PRECISION_FOR_FLOAT_TO_STRING 4
#endif

#define PORT_RATE  400//端口速率  bit/ns
#define HOST_PORT_RATE  6000//端口速率  bit/ns
#define PACKET_SIZE  4176//数据包大小  Byte
#define ACK_SIZE  70//ACK大小  Byte
#define LINK_DELAY  20    //链路传播延迟  ns
namespace ns3 {

class UbRoutingProcess;
    template <typename T>
std::string ToString(T src) {
  std::stringstream ss;
  ss << std::fixed << std::setprecision(DEFAULT_PRECISION_FOR_FLOAT_TO_STRING);
  ss << src;
  std::string str = ss.str();
  return str;
}

template <typename T>
std::string VectorToString (std::vector<T> src) {
  std::string str = "[";
  uint32_t vecSize = src.size();
  if (vecSize == 0) {
    str = str + "NULL]";
    return str;
  }else{
    for (uint32_t i = 0; i < vecSize-1; i++) {
      std::string curStr = ToString<T>(src[i]);
      str = str + curStr + ", ";
    }
    std::string curStr = ToString<T>(src[vecSize-1]);
    str = str + curStr + "]";
    return str;
  }
}

/**
 * @brief ALPS PIT表项，记录每个src-dst对应的ALPS路径ID列表
 */
class AlpsPitEntry
{
    uint32_t pathId;     // ALPS路径ID
    uint32_t length;   // 路径长度
    uint32_t reversePathId; // 反向路径ID
    std::vector<uint32_t> nodes;  // ALPS路径对应的入端口列表
    std::vector<uint32_t> ports; // ALPS路径对应的出端口列表
    uint32_t baseLatency; // ALPS路径的基准时延，路径时延超过此值即视为拥塞
    uint32_t noQueueLatencyNs; // ALPS路径的无队列时延，不包含排队时延等
    Time lastUpdatedTime; // 上次更新时间
    Time lastUsedTime; // 上次使用时间
    Time lastProbeTime; // 上次探测时间
    double virtualLatencyNs; // 仅用于负载均衡的虚时延
    double weight;
    static bool s_enableVirtualLatency;
    static bool s_enablePathWeight;
    public:
    void SetPathId(uint32_t pathId) { this->pathId = pathId; }
    uint32_t GetPathId() const { return pathId; }

    void SetLength(uint32_t length) { this->length = length; }
    uint32_t GetLength() const { return length; }

    void SetReversePathId(uint32_t reversePathId) { this->reversePathId = reversePathId; }
    uint32_t GetReversePathId() const { return reversePathId; }

    void SetNodes(const std::vector<uint32_t>& nodes) { this->nodes = nodes; }
    const std::vector<uint32_t>& GetNodes() const { return nodes; }

    void SetPorts(const std::vector<uint32_t>& ports) { this->ports = ports; }
    const std::vector<uint32_t>& GetPorts() const { return ports; }

    void SetBaseLatency(uint32_t baseLatency) { this->baseLatency = baseLatency; }
    uint32_t GetBaseLatency() const { return baseLatency; }

    void SetNoQueueLatency(uint32_t noQueueLatency) { this->noQueueLatencyNs = noQueueLatency; }
    uint32_t GetNoQueueLatency() const { return noQueueLatencyNs; }
    uint32_t GetRealTimeLatency(Ptr<UbRoutingProcess> ubRoutingProcess) const ;
    void UpdateLastUpdatedTime(Time lastUpdatedTime) { this->lastUpdatedTime = lastUpdatedTime; }
    Time GetLastUpdatedTime() const { return lastUpdatedTime; }

    void UpdateLastUsedTime(Time lastUsedTime) { this->lastUsedTime = lastUsedTime; }
    Time GetLastUsedTime() const { return lastUsedTime; }
    void UpdateLastProbeTime(Time lastProbeTime) { this->lastProbeTime = lastProbeTime; }
    Time GetLastProbeTime() const { return lastProbeTime; }
    static void SetEnableVirtualLatency(bool enable) { s_enableVirtualLatency = enable; }
    static bool IsVirtualLatencyEnabled() { return s_enableVirtualLatency; }
    static void SetEnablePathWeight(bool enable) { s_enablePathWeight = enable; }
    static bool IsPathWeightEnabled() { return s_enablePathWeight; }
    static void InitializeFeatureSwitchesFromConfig();
    double GetVirtualLatencyNs() const { return virtualLatencyNs; }
    void ResetVirtualLatency() { virtualLatencyNs = 0.0; }
    void AddVirtualLatencyByPacketSize(uint32_t packetSizeBytes)
    {
      if (!s_enableVirtualLatency) {
        return;
      }
      virtualLatencyNs += (static_cast<double>(packetSizeBytes) * 8.0) / static_cast<double>(PORT_RATE);
    }
    double GetWeight() const {
      if(!s_enablePathWeight){
      return 1.0;
      }
      return weight; 
    }
    AlpsPitEntry() : pathId(0), length(0), reversePathId(0), baseLatency(0), noQueueLatencyNs(0)
    {
        nodes.clear();
        ports.clear();
        lastUpdatedTime = Seconds(0);
        lastProbeTime = Seconds(0);
        virtualLatencyNs = 0.0;
    }
    AlpsPitEntry(uint32_t pathId, uint32_t length, std::vector<uint32_t> nodes, std::vector<uint32_t> ports, uint32_t reversePathId, uint32_t m_baseLatency, double weight) : pathId(pathId), length(length), reversePathId(reversePathId), nodes(nodes), ports(ports), weight(weight)
    {
        lastUpdatedTime = Seconds(0);
        lastProbeTime = Seconds(0);
        virtualLatencyNs = 0.0;
        //调整maxBaseDelay的时候m_nextSlowdownTime也会进行变动，所以需要再次对调速间隔进行评估
        baseLatency = m_baseLatency+((length-2)*(PACKET_SIZE*8)/(PORT_RATE)+2*(PACKET_SIZE*8)/(HOST_PORT_RATE))*10; // 
        noQueueLatencyNs = m_baseLatency+(length-2)*(PACKET_SIZE*8)/(PORT_RATE)+2*(PACKET_SIZE*8)/(HOST_PORT_RATE);
    }
    

    void Print(std::ostream& os) const
    {
        os << "PathID:" << pathId << ", length:" << length << ", reversePathID:" << reversePathId << ", baseLatency:" << baseLatency << ", noQueueLatency:" << noQueueLatencyNs << ", nodes:" << VectorToString(nodes) << ", ports:" << VectorToString(ports)<<", weight:"<< weight;
    }
    std::string ToString() const
    {
        return "[PathID:" + std::to_string(pathId) + ", length:" + std::to_string(length) + ", reversePathID:" + std::to_string(reversePathId) + ", baseLatency:" + std::to_string(baseLatency) + ", noQueueLatency:" + std::to_string(noQueueLatencyNs) + ", nodes:" + VectorToString(nodes) + ", ports:" + VectorToString(ports) + ", weight:" + std::to_string(weight) + "]";
    }
};



/**
 * @brief ALPS PST表项，记录每个src-dst对应的ALPS路径ID列表
 */
struct AlpsPstEntry
{
    uint32_t srcNodeId;
    uint32_t dstNodeId;
    uint32_t num_paths;
    std::vector<AlpsPitEntry*> PitEntries; // ALPS路径ID列表
    AlpsPstEntry() : srcNodeId(0), dstNodeId(0), num_paths(0) { PitEntries.clear(); }
    AlpsPstEntry(uint32_t src, uint32_t dst, uint32_t num, std::vector<AlpsPitEntry*> paths) : srcNodeId(src), dstNodeId(dst), num_paths(num), PitEntries(paths)
    {
    }
    void AddPitEntry(AlpsPitEntry*PitEntry)
    {
        PitEntries.push_back(PitEntry);
        num_paths = PitEntries.size();
    }
    void Print(std::ostream& os) const
    {
      std::vector<uint32_t> ids;
      ids.reserve(PitEntries.size());
      for (const auto &p : PitEntries) {
        ids.push_back(p ? p->GetPathId() : 0);
      }
      os << "srcNode:" << srcNodeId << ", dstNode:" << dstNodeId << ", num_paths:" << num_paths << ", pathIds:" << VectorToString(ids);
    }
    std::string ToString() const
    {
      std::vector<uint32_t> ids;
      ids.reserve(PitEntries.size());
      std::vector<uint32_t> reserveids;
      for (const auto &p : PitEntries) {

        ids.push_back(p ? p->GetPathId() : 0);
        reserveids.push_back(p ? p->GetReversePathId() : 0);
      }
      return "[srcNode:" + std::to_string(srcNodeId) + ", dstNode:" + std::to_string(dstNodeId) + ", num_paths:" + std::to_string(num_paths) + ", pathIds:" + VectorToString(ids) + ", reversePathIds:" + VectorToString(reserveids) + "]";
    }
};


}



#endif // UB_ALPS_PST_H