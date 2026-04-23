// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_ALPS_H
#define UB_ALPS_H
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <time.h>
#include <random>
#include "ns3/ub-congestion-control.h"
#include "ns3/ptr.h"
#include "ns3/packet.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-header.h"
#include "ns3/simulator.h"
#include "ns3/data-rate.h"
#include "ns3/random-variable-stream.h"
#include "ns3/ub-alps-pst.h"
namespace ns3 {
class UbCongestionExtTph;
class UbNetworkHeader;
class UbSwitch;
class UbTransportChannel;

#define DEFAULT_PRECISION_FOR_FLOAT_TO_STRING 4

/**
 * @brief Alps algo, inherit from UbCongestionControl,
 * define some params that alps algo used.
 */
class UbAlps : public UbCongestionControl {
public:
    UbAlps();
    ~UbAlps() override;
    static TypeId GetTypeId(void);

protected:
    UbNodeType_t m_nodeType;    // 所属节点的类型
    //全部没有用到，先保留着
    double m_alpha;             // 拥塞避免阶段增窗系数，α / cwnd，
    double m_beta;              // 减窗系数，β * mtu，
    double m_gamma;             // 窗口下限系数， γ * mtu
    uint32_t m_theta;           // 状态重置时间系数，θ * rtt时间内没有收到阻塞 or 拒绝增窗ack，状态重置为慢启动
    double m_lambda;            // CC计算系数

    uint32_t m_idealQueueSize;  // 期望的最大队列size
    uint32_t m_ccUnit;          // 慢启动阶段一个hint表示的字节数
    double m_markProbability;   // CC大于零但不足时包被标记的概率
};

/**
 * @brief Alps algo host part.
 */
class UbHostAlps : public UbAlps {
public:
    UbHostAlps();
    ~UbHostAlps() override;
    static TypeId GetTypeId(void);

    // 初始化
    void TpInit(Ptr<UbTransportChannel> tp) override;

    // 获取剩余窗口，ALPS LDCP需要
    uint32_t GetRestCwnd() override;
     
    // 发送端生成拥塞控制算法需要的header
    UbNetworkHeader SenderGenNetworkHeader() override;

    // 发送端发包，更新数据
    void SenderUpdateCongestionCtrlData(uint32_t psn, uint32_t size) override;

    // 接收端接到数据包后记录数据
    void RecverRecordPacketData(uint32_t psn, uint32_t size, UbNetworkHeader header) override;

    // 接收端生成拥塞控制算法需要的ack header
    UbCongestionExtTph RecverGenAckCeTphHeader(uint32_t psnStart, uint32_t psnEnd) override;
    // 发送端收到ack，调整窗口、速率等数据
    void SenderRecvAck(uint32_t psn, UbCongestionExtTph header) override;
   
    void UpdateNextSendTime(uint32_t pktsize,uint32_t port);
     DataRate GetRate() { return m_currentRate; }
     
    Time GetNextSendTime();
    void UpdateLastVisitTime(Time time, string reason){
        m_LastVisitTime = time;
        m_lastVisitReason = reason;
    }
    void UpdateNextSendTimeForRateAdjustment(uint32_t RemaintransmissionDelayinNs);
    Time GetLastVisitTime() { return m_LastVisitTime; }
    string GetLastVisitReason() { return m_lastVisitReason; }
    bool IsBdpLikeFull(uint32_t nextPacketBytes = 0) const;
    void AddBdpLikeInFlightBytes(uint32_t packetBytes);
    void AckBdpLikeInFlightBytes(uint32_t packetBytes);
    void  AckPerPathBdpInFlightBytes(uint32_t pid, uint32_t packetSize);
private:
    void StateReset();
    void InitRateControlState();
    void InitFixedPathLatencyForBdp(uint32_t RateinBps);
    void RefreshBdpLikeLimitAfterRateChange();
    bool TrySpeedUpForALPS(Time maxBaseDelay);
    bool TrySlowDownForALPS(Time maxBaseDelay);

    void DoDispose() override;

    uint32_t m_src;
    uint32_t m_dst;
    uint32_t m_tpn;

    enum CongestionState {
        SLOW_START,
        CONGESTION_AVOIDANCE
    };
    CongestionState m_congestionState = SLOW_START;
    //没用到
    uint32_t    m_dataByteSent = 0;     // 总计发送数据量
    uint32_t    m_dataByteRecvd = 0;    // 总计收到数据量
    uint32_t    m_inFlight = 0;         // 已发送但还没有收到ack的数据量

    uint32_t    m_cwnd;                 // 发送窗口大小
    uint32_t    m_lastSequence = 0;

    DataRate m_currentRate;             // 当前发送速率
    DataRate m_baseRate;                // 基准速率
    DataRate m_maxRate;                 // 最大速率（网卡上限）
    DataRate m_minRate;                 // 最小速率（硬限制）

    Time m_nextSlowdownTime = Seconds(0); // 下一次允许减速时间
    Time m_nextSpeedupTime = Seconds(0);  // 下一次允许加速时间

    uint32_t m_consecutiveSpeedups = 0; // 连续加速次数

    bool m_rateLimitEnabled = true;     // 是否启用速率限制
    Time m_nextSendTime = Seconds(0);   // 下次允许发送时间
    Time m_LastVisitTime = Seconds(0);   //最后一次访问时间
    string m_lastVisitReason = "";

    bool m_bdpLimitEnabled = false;
    uint64_t m_fixedPathLatencyForBdpNs = 0;
    uint64_t m_bdpLikeLimitBits = 0;
    uint64_t m_bdpLikeInFlightBits = 0;

    EventId m_nextSendTimerEvent{};     // 下次发送定时器事件 ID
};

/**
 * @brief Alps algo switch part.
 */
class UbSwitchAlps : public UbAlps {
public:
    static TypeId GetTypeId(void);
    UbSwitchAlps();
    ~UbSwitchAlps() override;
    // 初始化
    void SwitchInit(Ptr<UbSwitch> sw) override;

    // 设置每个端口的带宽
    void SetDataRate(uint32_t portId, DataRate bps);

    // 交换机收到包进行转发，对其进行处理
    void SwitchForwardPacket(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) override;

    // switch自动更新cc
    void ResetLocalCc();

private:

    void DoDispose() override;

    Time m_ccUpdatePeriod;                      // 交换机自动更新CC的周期
    std::vector<int64_t> m_cc;                  // Credit Counter，端口可用信用证的数量，代表端口空闲转发能力
    std::vector<uint64_t> m_txSize ;            // 实际吞吐量
    std::vector<int64_t> m_DC;                  // Deficit Counter，赤字计数器
    std::vector<int64_t> m_creditAllocated;     // 上一次循环中分配除去的信用证
    std::vector<DataRate> m_bps;                // port带宽

    uint32_t m_nodeId;                          // 绑定的switch节点号

    Ptr<UniformRandomVariable> m_random;        // 随机数产生工具，伪随机，多次仿真可复现
};





// /**
//  * @brief Alps algo, inherit from UbCongestionControl,
//  * define some params that alps algo used.
//  */
// class UbAlps : public UbCongestionControl {
// public:
//     UbAlps();
//     ~UbAlps() override;
//     static TypeId GetTypeId(void);

// protected:
//     UbNodeType_t m_nodeType;    // 所属节点的类型

//     double m_alpha;             // 拥塞避免阶段增窗系数，α / cwnd，
//     double m_beta;              // 减窗系数，β * mtu，
//     double m_gamma;             // 窗口下限系数， γ * mtu
//     uint32_t m_theta;           // 状态重置时间系数，θ * rtt时间内没有收到阻塞 or 拒绝增窗ack，状态重置为慢启动
//     double m_lambda;            // CC计算系数

//     uint32_t m_idealQueueSize;  // 期望的最大队列size
//     uint32_t m_ccUnit;          // 慢启动阶段一个hint表示的字节数
//     double m_markProbability;   // CC大于零但不足时包被标记的概率
// };

// /**
//  * @brief Alps algo host part.
//  */
// class UbHostAlps : public UbAlps {
// public:
//     UbHostAlps();
//     ~UbHostAlps() override;
//     static TypeId GetTypeId(void);

//     // 初始化
//     void TpInit(Ptr<UbTransportChannel> tp) override;

//     // 获取剩余窗口，ALPS LDCP需要
//     uint32_t GetRestCwnd() override;

//     // 发送端生成拥塞控制算法需要的header
//     UbNetworkHeader SenderGenNetworkHeader() override;

//     // 发送端发包，更新数据
//     void SenderUpdateCongestionCtrlData(uint32_t psn, uint32_t size) override;

//     // 接收端接到数据包后记录数据
//     void RecverRecordPacketData(uint32_t psn, uint32_t size, UbNetworkHeader header) override;

//     // 接收端生成拥塞控制算法需要的ack header
//     UbCongestionExtTph RecverGenAckCeTphHeader(uint32_t psnStart, uint32_t psnEnd) override;

//     // 发送端收到ack，调整窗口、速率等数据
//     void SenderRecvAck(uint32_t psn, UbCongestionExtTph header) override;

// private:
//     void StateReset();

//     void DoDispose() override;

//     uint32_t m_src;
//     uint32_t m_dst;
//     uint32_t m_tpn;

//     enum CongestionState {
//         SLOW_START,
//         CONGESTION_AVOIDANCE
//     };
//     CongestionState m_congestionState = SLOW_START;

//     uint32_t    m_dataByteSent = 0;     // 总计发送数据量
//     uint32_t    m_dataByteRecvd = 0;    // 总计收到数据量
//     uint32_t    m_inFlight = 0;         // 已发送但还没有收到ack的数据量
//     uint32_t    m_cwnd;                 // 发送窗口大小

//     uint32_t    m_lastSequence = 0;

//     std::unordered_map<uint32_t, uint32_t> m_recvdPsnPacketSizeMap;
//     std::unordered_map<uint32_t, uint16_t> m_recvdPsnHintMap;
//     std::unordered_map<uint32_t, uint8_t> m_recvdPsnCMap;
//     std::unordered_map<uint32_t, uint8_t> m_recvdPsnIMap;

//     std::unordered_map<uint32_t, Time> m_psnSendTimeMap;

//     Time m_rtt = NanoSeconds(0);
//     EventId m_congestionStateResetEvent{};

//     uint16_t m_HintE = 0;           // 聚合hint
//     uint8_t m_CE = 0;               // 聚合ce
//     uint8_t m_IE = 0;               // 聚合ie
//     double m_accumulateHint = 0;    // 累计的增窗请求之和，大于1时才会将hint设置数字
// };

// /**
//  * @brief Alps algo switch part.
//  */
// class UbSwitchAlps : public UbAlps {
// public:
//     static TypeId GetTypeId(void);
//     UbSwitchAlps();
//     ~UbSwitchAlps() override;
//     // 初始化
//     void SwitchInit(Ptr<UbSwitch> sw) override;

//     // 设置每个端口的带宽
//     void SetDataRate(uint32_t portId, DataRate bps);

//     // 交换机收到包进行转发，对其进行处理
//     void SwitchForwardPacket(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) override;

//     // switch自动更新cc
//     void ResetLocalCc();

// private:

//     void DoDispose() override;

//     Time m_ccUpdatePeriod;                      // 交换机自动更新CC的周期
//     std::vector<int64_t> m_cc;                  // Credit Counter，端口可用信用证的数量，代表端口空闲转发能力
//     std::vector<uint64_t> m_txSize ;            // 实际吞吐量
//     std::vector<int64_t> m_DC;                  // Deficit Counter，赤字计数器
//     std::vector<int64_t> m_creditAllocated;     // 上一次循环中分配除去的信用证
//     std::vector<DataRate> m_bps;                // port带宽

//     uint32_t m_nodeId;                          // 绑定的switch节点号

//     Ptr<UniformRandomVariable> m_random;        // 随机数产生工具，伪随机，多次仿真可复现
// };
}
#endif