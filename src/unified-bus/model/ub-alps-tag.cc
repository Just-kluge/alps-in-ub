#include "ns3/tag.h"
#include "ns3/ub-alps-tag.h"

namespace ns3 {
    
    NS_LOG_COMPONENT_DEFINE("UbAlpsTag");
    NS_OBJECT_ENSURE_REGISTERED(UbAlpsTag);

    UbAlpsTag::UbAlpsTag ()
    {
        NS_LOG_FUNCTION(this);
    }

    UbAlpsTag::~UbAlpsTag ()
    {
        NS_LOG_FUNCTION(this);
    }


    TypeId UbAlpsTag::GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbAlpsTag")
                                .SetParent<Tag>()
                                .AddConstructor<UbAlpsTag>();
        return tid;
    }

    TypeId UbAlpsTag::GetInstanceTypeId (void) const
    {
        return GetTypeId ();
    }

    uint32_t UbAlpsTag::GetSerializedSize (void) const
    {
        return   sizeof(m_srInfo.pathId)
               + sizeof(m_srInfo.hopCnt)
               + sizeof(m_pathLength)
               + sizeof(m_direction)
               + sizeof(m_type)
               + sizeof(m_timeStamp)
               + sizeof(m_rateInHundredGbps)
               + sizeof(m_scaledAccQlenInKB)
               + sizeof(m_ackPsn)
               + sizeof(uint32_t)
               + static_cast<uint32_t>(8) * sizeof(uint32_t);
    }

    void UbAlpsTag::Serialize (TagBuffer i) const
    {
        i.WriteU32 (m_srInfo.pathId);
        i.WriteU8 (m_srInfo.hopCnt);
        i.WriteU16 (m_pathLength);
        i.WriteU8 (m_direction);
        i.WriteU8 (m_type);
        i.WriteDouble (m_timeStamp.GetNanoSeconds());
        i.WriteU8 (m_rateInHundredGbps);
        i.WriteU32 (m_scaledAccQlenInKB);
        i.WriteU32 (m_ackPsn);
        i.WriteU32 (static_cast<uint32_t>(m_queueingDelayNanoSecondsList.size()));
        for (uint32_t delayNs : m_queueingDelayNanoSecondsList) {
            i.WriteU32(delayNs);
        }
        return;  
    }

    void UbAlpsTag::Deserialize (TagBuffer i) {
        m_srInfo.pathId = i.ReadU32 ();
        m_srInfo.hopCnt = i.ReadU8 ();
        m_pathLength = i.ReadU16 ();
        m_direction = i.ReadU8 ();
        m_type = i.ReadU8 ();
        m_timeStamp = Time::FromDouble (i.ReadDouble (), Time::NS);
        m_rateInHundredGbps = i.ReadU8 ();
        m_scaledAccQlenInKB = i.ReadU32 ();
        m_ackPsn = i.ReadU32 ();
        uint32_t listSize = i.ReadU32();
        m_queueingDelayNanoSecondsList.clear();
        m_queueingDelayNanoSecondsList.reserve(listSize);
        for (uint32_t idx = 0; idx < listSize; ++idx) {
            m_queueingDelayNanoSecondsList.push_back(i.ReadU32());
        }
        return;
    }

    void UbAlpsTag::Print (std::ostream &os) const
    {
        os << "UbAlpsTag: " << "SRInfo=";
        m_srInfo.Print(os);
        os << ", Dir=";
        if (m_direction == 0) {
            os << "data";
        } else if (m_direction == 1) {
            os << "ack";
        } else {
            os << "unknown(" << (uint32_t)m_direction << ")";
        }
        os << ", Type=";
        if (m_type == 0) {
            os << "normal";
        } else if (m_type == 1) {
            os << "probe";
        } else {
            os << "unknown(" << (uint32_t)m_type << ")";
        }
        os << ", TimeStamp=" << m_timeStamp.GetNanoSeconds() << "ns";
        os << ", PathLength=" << m_pathLength;
        os << ", Rate=" << (uint32_t)m_rateInHundredGbps * 100 << "Gbps";
        os << ", ScaledAccQlen=" << m_scaledAccQlenInKB << "KB";
        os << ", AckPsn=" << m_ackPsn;
        os << ", QueueDelayListNs=[";
        for (size_t i = 0; i < m_queueingDelayNanoSecondsList.size(); ++i) {
            os << m_queueingDelayNanoSecondsList[i];
            if (i + 1 < m_queueingDelayNanoSecondsList.size()) {
                os << ",";
            }
        }
        os << "]";
        return;
    }

    // Setters
    void UbAlpsTag::SetSrInfo (UbAlpsSrInfo srInfo)
    {
        m_srInfo = srInfo;
    }
    void UbAlpsTag::SetDirection (uint8_t direction)
    {
        m_direction = direction;
    }
    void UbAlpsTag::SetType (uint8_t type)
    {
        m_type = type;
    }
    void UbAlpsTag::SetTimeStamp (Time timeStamp)
    {
        m_timeStamp = timeStamp;
    }
    void UbAlpsTag::SetRateInHundredGbps (uint8_t rate)
    {
        m_rateInHundredGbps = rate;
    }
    void UbAlpsTag::SetScaledAccQlenInKB (uint32_t qlen)
    {
        m_scaledAccQlenInKB = qlen;
    }
    void UbAlpsTag::SetAckPsn(uint32_t ackPsn)
    {
        m_ackPsn = ackPsn;
    }
    void UbAlpsTag::AppendQueueingDelayNanoSeconds(uint32_t delayNs)
    {
        m_queueingDelayNanoSecondsList.push_back(delayNs);
    }
    void UbAlpsTag::ClearQueueingDelayNanoSecondsList()
    {
        m_queueingDelayNanoSecondsList.clear();
    }
    void UbAlpsTag::SetHopCount(uint8_t hopCnt)
    {
        m_srInfo.hopCnt = hopCnt;
    }
    void UbAlpsTag::SetPathId(uint32_t pathId)
    {
        m_srInfo.pathId = pathId;
    }
    void UbAlpsTag::SetPathLength(uint16_t pathLength)
    {
        m_pathLength = pathLength;
    }

    // Getters
    UbAlpsSrInfo UbAlpsTag::GetSrInfo (void) const
    {
        return m_srInfo;
    }
    uint8_t UbAlpsTag::GetHopCount() const
    {
        return m_srInfo.hopCnt;
    }
    uint32_t UbAlpsTag::GetPathId() const
    {
        return m_srInfo.pathId;
    }
    uint16_t UbAlpsTag::GetPathLength() const
    {
        return m_pathLength;
    }
    uint8_t UbAlpsTag::GetDirection (void) const
    {
        return m_direction;
    }
    uint8_t UbAlpsTag::GetType (void) const
    {
        return m_type;
    }
    Time UbAlpsTag::GetTimeStamp (void) const
    {
        return m_timeStamp;
    }
    uint8_t UbAlpsTag::GetRateInHundredGbps (void) const
    {
        return m_rateInHundredGbps;
    }
    uint32_t UbAlpsTag::GetScaledAccQlenInKB (void) const
    {
        return m_scaledAccQlenInKB;
    }
    uint32_t UbAlpsTag::GetAckPsn() const
    {
        return m_ackPsn;
    }
    const std::vector<uint32_t>& UbAlpsTag::GetQueueingDelayNanoSecondsList() const
    {
        return m_queueingDelayNanoSecondsList;
    }



}