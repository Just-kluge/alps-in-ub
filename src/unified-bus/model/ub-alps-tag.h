// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_ALPS_TAG_H
#define UB_ALPS_TAG_H
#include <unordered_map>
#include <vector>
#include <ns3/tag.h>
#include "ns3/nstime.h"

namespace ns3 {



/**
  * @brief struct for ALPS SR information, including pathId and hopCnt.
  */
struct UbAlpsSrInfo {
    uint32_t pathId;
    uint8_t hopCnt;
    UbAlpsSrInfo() : pathId(0), hopCnt(0) {}
    UbAlpsSrInfo(uint32_t id, uint8_t cnt) : pathId(id), hopCnt(cnt) {}
    void Print(std::ostream& os) const
    {
        os << "pathId:" << pathId << ", hopCnt:" << (uint32_t)hopCnt;
    }
    std::string ToString() const
    {
        return "[" + std::to_string(pathId) + ", " + std::to_string(hopCnt) + "]";
    }
};

/**
  * @brief Tag for ALPS congestion control, attached to packets for recording necessary information for ALPS algo.
  */

class UbAlpsTag : public Tag
{
public:
    UbAlpsTag ();
    virtual ~UbAlpsTag ();

    static TypeId GetTypeId (void);
    virtual TypeId GetInstanceTypeId (void) const;
    virtual uint32_t GetSerializedSize (void) const;
    virtual void Serialize (TagBuffer i) const;
    virtual void Deserialize (TagBuffer i);
    virtual void Print (std::ostream &os) const;

    void SetSrInfo (UbAlpsSrInfo srInfo);
    UbAlpsSrInfo GetSrInfo (void) const;
    void SetHopCount(uint8_t hopCnt);
    uint8_t GetHopCount() const;
    void SetPathId(uint32_t pathId);
    uint32_t GetPathId() const;
    void SetPathLength(uint16_t pathLength);
    uint16_t GetPathLength() const;
    void SetDirection (uint8_t direction);
    uint8_t GetDirection (void) const;
    void SetType (uint8_t type);
    uint8_t GetType (void) const;
    void SetTimeStamp (Time timeStamp);
    Time GetTimeStamp (void) const;
    void SetRateInHundredGbps (uint8_t rate);
    uint8_t GetRateInHundredGbps (void) const;
    void SetScaledAccQlenInKB (uint32_t qlen);
    uint32_t GetScaledAccQlenInKB (void) const;
    void SetAckPsn(uint32_t ackPsn);
    uint32_t GetAckPsn() const;
    void AppendQueueingDelayNanoSeconds(uint32_t delayNs);
    const std::vector<uint32_t>& GetQueueingDelayNanoSecondsList() const;
    void ClearQueueingDelayNanoSecondsList();

private:
    // 5 + 1 + 1 + 8 + 1 + 4 = 20 bytes
    UbAlpsSrInfo m_srInfo; // 5 bytes (40 bits)
    uint16_t m_pathLength {0}; // 数据包路径总跳数，仅用于统计
    uint8_t m_direction; // 0: data, 1: ack. 1 bit
    uint8_t m_type; // 0: normal, 1: probe. 1 bit
    Time m_timeStamp; // 8 bytes (64 bits)
    uint8_t m_rateInHundredGbps; // 发送端网卡速率，单位为100Gbps. 1 byte (8 bits)
    uint32_t m_scaledAccQlenInKB; // scaled accumulated queue length in KB, for ALPS congestion control. 4 bytes (32 bits)
    uint32_t m_ackPsn {0}; // Packet-level ACK PSN used by ALPS sender matching.
    std::vector<uint32_t> m_queueingDelayNanoSecondsList; // ACK 返程沿途各交换机追加的排队时延列表（单位ns）
};

}
#endif