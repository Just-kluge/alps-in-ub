// SPDX-License-Identifier: GPL-2.0-only
#include <fstream>
#include <sstream>

#include "ns3/log.h"
#include "ns3/simulator.h"

#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-monitor.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-function.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbFunction");

NS_OBJECT_ENSURE_REGISTERED(UbFunction);

TypeId UbFunction::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFunction")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus");
    return tid;
}

UbFunction::UbFunction()
{
    NS_LOG_DEBUG("UbFunction created");
    m_ldstApi = CreateObject<UbLdstApi>();
}

UbFunction::~UbFunction()
{
    m_numToJetty.clear();
}

Ptr<UbLdstApi> UbFunction::GetUbLdstApi()
{
    return m_ldstApi;
}

void UbFunction::Init(uint32_t nodeId)
{
    m_nodeId = nodeId;
    m_ldstApi->SetNodeId(m_nodeId);
}

void UbFunction::CreateJetty(uint32_t src, uint32_t dest, uint32_t jettyNum)
{
    NS_LOG_DEBUG(this << src << dest << jettyNum);
    // 创建新的Jetty
    Ptr<UbJetty> jetty = CreateObject<UbJetty>();
    jetty->Init();
    jetty->SetJettyNum(jettyNum);
    jetty->SetSrc(src);
    jetty->SetDest(dest);
    m_numToJetty[jettyNum] = jetty;
    NS_LOG_DEBUG("Created jetty success");
}

bool UbFunction::IsJettyExists(uint32_t jettyNum)
{
    if (m_numToJetty.find(jettyNum) != m_numToJetty.end()) {
        return true;
    }
    return false;
}

Ptr<UbJetty> UbFunction::GetJetty(uint32_t jettyNum)
{
    auto it = m_numToJetty.find(jettyNum);
    if (it != m_numToJetty.end()) {
        return it->second;
    }

    NS_LOG_DEBUG("Jetty not found");
    return nullptr;
}

Ptr<UbTransaction> UbFunction::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

void UbFunction::DestroyJetty(uint32_t jettyNum)
{
    NS_LOG_DEBUG(this);
    auto it = m_numToJetty.find(jettyNum);
    if (it != m_numToJetty.end()) {
        m_numToJetty.erase(it);
        NS_LOG_DEBUG("Destroyed ");
    } else {
        NS_LOG_WARN("Jetty  not found for destruction");
    }

    // 删除m_jettyVector中元素
    for (size_t i = 0; i < m_jettyVector.size(); i++) {
        if (m_jettyVector[i]->GetJettyNum() == jettyNum) {
            m_jettyVector.erase(m_jettyVector.begin() + i);
            break;
        }
    }
}

bool UbFunction::DumpUnfinishedJettyReport(const std::string& filePath) const
{
    std::ofstream out(filePath.c_str(), std::ios::out | std::ios::app);
    if (!out.is_open()) {
        NS_LOG_WARN("Failed to open jetty monitor log: " << filePath);
        return false;
    }

    uint32_t unfinishedCount = 0;
    for (const auto& item : m_numToJetty) {
        const Ptr<UbJetty>& jetty = item.second;
        if (!jetty || !jetty->HasPendingWqe()) {
            continue;
        }
        unfinishedCount++;
    }

    if (unfinishedCount == 0) {
        return false;
    }

    out << "================================================================================\n";
    out << "[JettyMonitor] nodeId=" << m_nodeId << " timeUs=" << Simulator::Now().GetMicroSeconds()
        << " unfinishedJettyCount=" << unfinishedCount << " totalJettyCount=" << m_numToJetty.size() << "\n";

    for (const auto& item : m_numToJetty) {
        const Ptr<UbJetty>& jetty = item.second;
        if (!jetty || !jetty->HasPendingWqe()) {
            continue;
        }

        jetty->WriteUnfinishedReport(out);
        out << "--------------------------------------------------------------------------------\n";
    }

    out << "================================================================================\n";
    return true;
}

Ptr<UbWqe> UbFunction::CreateWqe(uint32_t src, uint32_t dest, uint32_t size, uint32_t wqeId)
{
    NS_LOG_DEBUG(this);
    // 创建新的 WQE
    Ptr<UbWqe> ubWqe = CreateObject<UbWqe>();
    ubWqe->SetSrc(src);
    ubWqe->SetDest(dest);
    ubWqe->SetSize(size);
    ubWqe->SetWqeId(wqeId);
    return ubWqe;
}

void UbFunction::PushWqeToJetty(Ptr<UbWqe> wqe, uint32_t jettyNum)
{
    NS_LOG_DEBUG(this);
    Ptr<UbJetty> ubJetty = GetJetty(jettyNum);
    if (!ubJetty) {
        NS_LOG_WARN("Get jetty failed");
        return;
    }
    ubJetty->SetNodeId(m_nodeId);
    // 将 WQE 添加到 Jetty
    ubJetty->PushWqe(wqe);
    GetTransaction()->AddWqe(jettyNum, wqe);
    GetTransaction()->TriggerScheduleWqeSegment(jettyNum);
}

void UbFunction::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ldstApi = nullptr;
    for (auto i = m_jettyVector.begin(); i != m_jettyVector.end(); i++) {
        *i = nullptr;
    }
    m_jettyVector.clear();
    m_numToJetty.clear();

    Object::DoDispose();
}

NS_OBJECT_ENSURE_REGISTERED(UbJetty);

TypeId UbJetty::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbJetty")
                        .SetParent<Object>()
                        .SetGroupName("UnifiedBus")
                        .AddAttribute("JettyOooAckThreshold",
                                      "Jetty Out-of-Order Ack Threshold",
                                      UintegerValue(2048),
                                      MakeUintegerAccessor(&UbJetty::m_oooAckThreshold),
                                      MakeUintegerChecker<uint32_t>())
                        .AddAttribute("UbInflightMax",
                                      "jetty inflight max",
                                      UintegerValue(10000),
                                      MakeUintegerAccessor(&UbJetty::m_inflightMax),
                                      MakeUintegerChecker<uint32_t>());
    return tid;
}

UbJetty::UbJetty()
{
}

bool UbJetty::HasPendingWqe() const
{
    return !m_wqeVector.empty();
}

void UbJetty::Init()
{
    ResetSsnAckBitset(m_oooAckThreshold);
}

void UbJetty::SetClientCallback(Callback<void, uint32_t, uint32_t> cb)
{
    FinishCallback = cb;
}

void UbJetty::WriteUnfinishedReport(std::ostream& os) const
{
    os << "JettyNum=" << m_jettyNum << " nodeId=" << m_nodeId << " src=" << m_src << " dest=" << m_dest
       << " pendingWqeCount=" << m_wqeVector.size() << "\n";
    os << "  SSN: m_taMsnCnt=" << m_taMsnCnt << " m_taSsnCnt=" << m_taSsnCnt
       << " m_taSsnSndNxt=" << m_taSsnSndNxt << " m_taSsnSndUna=" << m_taSsnSndUna << "\n";

    const uint32_t generatedNotScheduled = (m_taSsnCnt >= m_taSsnSndNxt) ? (m_taSsnCnt - m_taSsnSndNxt) : 0;
    const uint32_t inflightUnacked = (m_taSsnSndNxt >= m_taSsnSndUna) ? (m_taSsnSndNxt - m_taSsnSndUna) : 0;
    os << "  Derived: generatedNotScheduled=" << generatedNotScheduled
       << " inflightUnacked=" << inflightUnacked << " acknowledged=" << m_taSsnSndUna << "\n";

    if (generatedNotScheduled > 0) {
        os << "  StallHint=DISPATCH_PENDING\n";
    } else if (inflightUnacked > 0) {
        os << "  StallHint=ACK_PENDING\n";
    } else {
        os << "  StallHint=UNKNOWN\n";
    }

    os << "  UnfinishedWqeList:\n";
    for (size_t i = 0; i < m_wqeVector.size(); ++i) {
        const Ptr<UbWqe>& wqe = m_wqeVector[i];
        if (!wqe) {
            os << "    WQE[" << i << "]: null\n";
            continue;
        }

        const uint64_t totalSize = wqe->GetSize();
        const uint64_t bytesLeft = wqe->GetBytesLeft();
        const uint64_t bytesSent = (totalSize >= bytesLeft) ? (totalSize - bytesLeft) : 0;
        const uint64_t ssnStart = wqe->GetTaSsnStart();
        const uint64_t ssnEnd = ssnStart + wqe->GetTaSsnSize() - 1;

        os << "    WQE[" << i << "]: taskId=" << wqe->GetWqeId() << " msn=" << wqe->GetTaMsn()
           << " ssnRange=[" << ssnStart << "~" << ssnEnd << "]"
           << " size=" << totalSize << " bytesSent=" << bytesSent << " bytesLeft=" << bytesLeft
           << " canSend=" << (wqe->CanSend() ? "true" : "false")
           << " sentCompleted=" << (wqe->IsSentCompleted() ? "true" : "false") << "\n";
    }
}


// ============================================================================
// UbJetty 实现
// ============================================================================
Ptr<UbTransaction> UbJetty::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

Ptr<UbWqeSegment> UbJetty::GetNextWqeSegment()
{
    NS_LOG_FUNCTION(this);

    if (IsLimited()) {
        NS_LOG_WARN("Inflight reach limit");
        return nullptr;
    }

    if (m_wqeVector.empty()) {
        NS_LOG_DEBUG("No WQE available to send");
        return nullptr;
    }

    // 获取第一个未完成的 WQE
    Ptr<UbWqe> currentWqe = nullptr;
    for (auto it = m_wqeVector.begin(); it != m_wqeVector.end(); ++it) {
        if (*it && !(*it)->IsSentCompleted()) {
            currentWqe = *it;
            // WQE初始发送状态为False，当TA根据事务序判断当前WQE可以发送后，修改为True，即可发送。
            if (currentWqe->CanSend()) {
                break;
            } else {
                // 调用TA，判断currentWqe是否允许发送
                auto ubTa = GetTransaction();
                if (ubTa->IsOrderedByInitiator(m_jettyNum, currentWqe)) {
                    currentWqe->SetCanSend(true);
                    break;
                }
            }
        }
    }

    if (!currentWqe) {
        NS_LOG_DEBUG("No unfinished WQE available to send");
        return nullptr;
    }

    if (!currentWqe->CanSend()) {
        NS_LOG_DEBUG("No unfinished WQE available to send");
        return nullptr;
    }

    uint64_t segmentSize = currentWqe->GetBytesLeft();
    if (segmentSize > UB_WQE_TA_SEGMENT_BYTE) {
        segmentSize = UB_WQE_TA_SEGMENT_BYTE;
    }

    Ptr<UbWqeSegment> segment = GenWqeSegment(currentWqe, segmentSize);
    segment->SetTaskId(currentWqe->GetWqeId());
    segment->SetWqeSize(currentWqe->GetSize());
    currentWqe->UpdateSentBytes(segmentSize);
    IncreaseTaSsnSndNxt(); // 更新下一个待发送的分段序号

    return segment;
}

Ptr<UbWqeSegment> UbJetty::GenWqeSegment(Ptr<UbWqe> wqe, uint32_t segmentSize)
{
    NS_LOG_FUNCTION(this << segmentSize);

    if (!wqe) {
        NS_LOG_ERROR("WQE is null");
        return nullptr;
    }

    if (segmentSize == 0) {
        NS_LOG_ERROR("Segment size is zero");
        return nullptr;
    }

    // 创建新的 WQE 分段
    Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();

    // 设置任务描述信息（从WQE复制）
    segment->SetSrc(wqe->GetSrc());
    segment->SetDest(wqe->GetDest());
    segment->SetSport(wqe->GetSport());
    segment->SetDport(wqe->GetDport());
    segment->SetType(wqe->GetType());
    segment->SetSize(segmentSize);
    segment->SetPriority(wqe->GetPriority());
    // 设置TA层信息
    segment->SetOrderType(wqe->GetOrderType());
    // 设置网络层信息（从WQE复制）
    segment->SetSip(wqe->GetSip());
    segment->SetDip(wqe->GetDip());

    // 设置TA层信息
    segment->SetJettyNum(wqe->GetJettyNum());
    segment->SetTaMsn(wqe->GetTaMsn());
    segment->SetTaSsn(m_taSsnSndNxt); // 使用当前的分段序号

    // TP layer information will be set during subsequent TP layer scheduling

    NS_LOG_DEBUG("Generated WQE segment: MSN=" << std::to_string(wqe->GetTaMsn())
                                              << ", SSN=" << std::to_string(m_taSsnSndNxt)
                                              << ", size=" << std::to_string(segmentSize)
                                              << ", src=" << std::to_string(wqe->GetSrc())
                                              << ", dest=" << std::to_string(wqe->GetDest()));

    return segment;
}

void UbJetty::PushWqe(Ptr<UbWqe> ubWqe)
{
    ubWqe->SetJettyNum(m_jettyNum);
    ubWqe->SetTaMsn(m_taMsnCnt);
    ubWqe->SetTaSsnStart(m_taSsnCnt);
    uint64_t ssnSize = (ubWqe->GetSize() + UB_WQE_TA_SEGMENT_BYTE - 1) / UB_WQE_TA_SEGMENT_BYTE;
    ubWqe->SetTaSsnSize(ssnSize);

    // 更新 Jetty 的 MSN 和 SSN 计数器
    m_taMsnCnt++;
    m_taSsnCnt += ssnSize;

    // 加入队列
    m_wqeVector.push_back(ubWqe);
    NS_LOG_INFO("WQE Starts, jettyNum:{" << std::to_string(m_jettyNum)
                <<  "} taskId:{" << std::to_string(ubWqe->GetWqeId()) << "}");
}

bool UbJetty::IsValid()
{
    return true;
}

bool UbJetty::IsReadyToSend()
{
    return true;
}

/*
 * TP调用这个方法，传入taSsnFin。
 */
bool UbJetty::ProcessWqeSegmentComplete(uint32_t taSsnAck)
{
    NS_LOG_DEBUG(this << taSsnAck);
    // 检查 SSN 是否在有效范围内
    if (taSsnAck < m_taSsnSndUna) {
        NS_LOG_WARN("Received ACK for already processed SSN " << std::to_string(taSsnAck)
                                                              << ", current m_taSsnSndUna is "
                                                              << std::to_string(m_taSsnSndUna));
        return false;
    }

    if (taSsnAck >= m_taSsnSndNxt) {
        NS_LOG_WARN("Received ACK for future SSN " << std::to_string(taSsnAck)
                                                   << ", current m_taSsnSndNxt is "
                                                   << std::to_string(m_taSsnSndNxt));
        return false;
    }

    if (IsLimited()) {
        GetTransaction()->TriggerTpTransmit(m_jettyNum);
    }

    // 计算在 bitset 中的相对位置
    uint32_t bitIndex = taSsnAck - m_taSsnSndUna;

    // 检查是否超出 bitset 范围
    if (bitIndex >= m_ssnAckBitset.size()) {
        NS_LOG_ERROR("SSN " << std::to_string(taSsnAck)
                            << " exceeds bitset capacity, bitIndex=" << std::to_string(bitIndex));
        return false;
    }

    // 设置对应的 bit
    m_ssnAckBitset[bitIndex] = true;

    NS_LOG_DEBUG("Set ACK bit for SSN " << std::to_string(taSsnAck) << " at bit index "
                                       << std::to_string(bitIndex));

    // 从最小未确认的 SSN 开始，连续更新已确认的分段
    uint32_t oldSndUna = m_taSsnSndUna;
    while (m_taSsnSndUna < m_taSsnSndNxt) {
        uint32_t currentBitIndex = m_taSsnSndUna - oldSndUna;
        if (currentBitIndex < m_ssnAckBitset.size() && m_ssnAckBitset[currentBitIndex]) {
            m_taSsnSndUna++;
        } else {
            break; // 遇到未确认的分段，停止
        }
    }

    // 如果 m_taSsnSndUna 有更新，需要清理 bitset
    if (m_taSsnSndUna > oldSndUna) {
        NS_LOG_DEBUG("Updated m_taSsnSndUna from " << std::to_string(oldSndUna) << " to "
                                                  << std::to_string(m_taSsnSndUna));

        // 手动右移 bitset
        uint32_t shiftCount = m_taSsnSndUna - oldSndUna;
        RightShiftBitset(shiftCount);

        // 检查并移除已完成的 WQE
        CheckAndRemoveCompletedWqe();
    }
    return true;
}

void UbJetty::RightShiftBitset(uint32_t shiftCount)
{
    if (shiftCount >= m_ssnAckBitset.size()) {
        std::fill(m_ssnAckBitset.begin(), m_ssnAckBitset.end(), false); // 清空所有位
        return;
    }

    // 手动实现右移
    for (size_t i = 0; i + shiftCount < m_ssnAckBitset.size(); ++i) {
        m_ssnAckBitset[i] = m_ssnAckBitset[i + shiftCount];
    }

    // 清空右移后的高位
    for (size_t i = m_ssnAckBitset.size() - shiftCount; i < m_ssnAckBitset.size(); ++i) {
        m_ssnAckBitset[i] = 0;
    }
}

void UbJetty::CheckAndRemoveCompletedWqe()
{
    NS_LOG_DEBUG(this);
    // 检查并移除已完成的WQE
    for (auto it = m_wqeVector.begin(); it != m_wqeVector.end();) {
        Ptr<UbWqe> wqe = *it;
        if (wqe && IsWqeCompleted(wqe)) {
            uint32_t wqeId = wqe->GetWqeId();
            NS_LOG_INFO("WQE Finishes, jettyNum: {" << m_jettyNum  << "} taskId:{ " << std::to_string(wqeId) <<"}");
            auto ubTa = GetTransaction();
            ubTa->WqeFinish(m_jettyNum, *it);
            // 从vector中移除已完成的WQE
            it = m_wqeVector.erase(it);
            FinishCallback(wqeId, m_jettyNum); // 调用应用层的回调
            // trigger tp
            ubTa->TriggerTpTransmit(m_jettyNum);
        } else {
            ++it;
        }
    }

    // 检查Jetty是否还有未完成的工作
    if (m_wqeVector.empty()) {
        NS_LOG_DEBUG("All WQEs in Jetty " << std::to_string(m_jettyNum) << " are completed");
    }
}

bool UbJetty::IsWqeCompleted(Ptr<UbWqe> wqe)
{
    NS_LOG_DEBUG(this);
    if (!wqe) {
        return false;
    }

    // 检查WQE的所有分段是否都已确认
    uint32_t wqeStartSsn = wqe->GetTaSsnStart();
    uint32_t wqeSsnSize = wqe->GetTaSsnSize();
    uint32_t wqeEndSsn = wqeStartSsn + wqeSsnSize - 1;

    // 如果WQE的最后一个分段已经被确认，则WQE完成
    return (wqeEndSsn < m_taSsnSndUna);
}

void UbJetty::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_wqeVector.clear();
    m_ssnAckBitset.clear();
    Object::DoDispose();
}


} // namespace ns3