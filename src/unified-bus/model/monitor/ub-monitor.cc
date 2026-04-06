// SPDX-License-Identifier: GPL-2.0-only

#include "ns3/ub-monitor.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include "ns3/ub-transport.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbMonitor");

std::unordered_map<uint32_t, UbAlpsPacketTracker::FlowTypeCounters>
	UbAlpsPacketTracker::s_nodeFlowTypeCounters;
std::unordered_map<uint32_t, std::unordered_map<uint32_t, UbAlpsPacketTracker::FlowTypeCounters>>
	UbAlpsPacketTracker::s_nodeTPFlowTypeCounters;
UbAlpsPacketTracker::GlobalDropStats UbAlpsPacketTracker::s_totalDropStats;
UbAlpsPacketTracker::GlobalDropStats UbAlpsPacketTracker::s_windowDropStats;
//===========true表示输出数据包全局信息打印到控制台中；=======================
bool UbAlpsPacketTracker::s_shouldPrintlog = true;
bool UbAlpsPacketTracker::s_dropStatsReportScheduled = true;
Time UbAlpsPacketTracker::s_lastDropStatsReportTime = Seconds(0);
uint64_t UbAlpsPacketTracker::s_totalSwitchDroppedPkts = 0;

std::unordered_map<uint32_t, UbAlpsNodeReceiveTracker::NodeReceiveCounters>
	UbAlpsNodeReceiveTracker::s_nodeReceiveCounters;
	//监测接收端接收数据的情况，用来分析是否匹配节点流量生成概率
bool UbAlpsNodeReceiveTracker::s_enableNodeReceiveTracker = true;
bool UbAlpsNodeReceiveTracker::s_reportScheduled = false;

static UbAlpsPacketTracker::FlowDropStats&
GetFlowDropStatsRef(UbAlpsPacketTracker::GlobalDropStats& stats, UbAlpsPacketTracker::FlowType type)
{
	if (type == UbAlpsPacketTracker::FlowType::SAME_COL)
	{
		return stats.sameCol;
	}
	if (type == UbAlpsPacketTracker::FlowType::SAME_RACK_CROSS_COL)
	{
		return stats.sameRackCrossCol;
	}
	return stats.crossRack;
}

static const char*
GetFlowTypeName(UbAlpsPacketTracker::FlowType type)
{
	if (type == UbAlpsPacketTracker::FlowType::SAME_COL)
	{
		return "same_col";
	}
	if (type == UbAlpsPacketTracker::FlowType::SAME_RACK_CROSS_COL)
	{
		return "same_rack_cross_col";
	}
	return "cross_rack";
}

void
UbAlpsPacketTracker::ResetGlobalFlowTypeStats()
{
	s_nodeFlowTypeCounters.clear();
	s_nodeTPFlowTypeCounters.clear();
	s_totalDropStats = GlobalDropStats{};
	s_windowDropStats = GlobalDropStats{};
	s_lastDropStatsReportTime = Seconds(0);
	s_totalSwitchDroppedPkts = 0;
}

UbAlpsPacketTracker::FlowType
UbAlpsPacketTracker::ClassifyFlowType(uint32_t srcNode, uint32_t dstNode)
{
	static const uint32_t kRackNum = 4;
	static const uint32_t kRackRows = 8;
	static const uint32_t kRackCols = 8;
	const uint32_t hostsPerRack = kRackRows * kRackCols;
	const uint32_t racksPerPod = kRackNum;
	const uint32_t hostsPerPod = hostsPerRack * racksPerPod;

	const uint32_t srcPod = srcNode / hostsPerPod;
	const uint32_t dstPod = dstNode / hostsPerPod;

	const uint32_t srcInPod = srcNode % hostsPerPod;
	const uint32_t dstInPod = dstNode % hostsPerPod;
	const uint32_t srcRack = srcInPod / hostsPerRack;
	const uint32_t dstRack = dstInPod / hostsPerRack;

	const uint32_t srcInRack = srcInPod % hostsPerRack;
	const uint32_t dstInRack = dstInPod % hostsPerRack;
	const uint32_t srcCol = srcInRack / kRackRows;
	const uint32_t dstCol = dstInRack / kRackRows;
	const uint32_t srcRow = srcInRack % kRackRows;
	const uint32_t dstRow = dstInRack % kRackRows;

	if (srcPod == dstPod && srcRack == dstRack)
	{
		if (srcCol == dstCol && srcRow != dstRow)
		{
			return FlowType::SAME_COL;
		}
		return FlowType::SAME_RACK_CROSS_COL;
	}
	return FlowType::CROSS_RACK;
}

void
UbAlpsPacketTracker::RecordPlannedFlow(uint32_t srcNode, uint32_t dstNode, uint32_t Taskid)
{
        if(s_shouldPrintlog){
		NS_LOG_UNCOND("[ALPS_INIT_CHECK] 当前速率初始化时会将奇数号Taskid排掉，因为奇数号Taskid被设置成combine阶段流，而目前初始化Dispatch阶段流不需要考虑combine阶段流 " << srcNode);
		s_shouldPrintlog = false;
	}
     //奇数号是combine的流，初始化时不考虑combine流数，当然后面还要考虑combine流的速率怎么设置
     if (Taskid %2!= 0)
     {
         return;
     }

	const FlowType type = ClassifyFlowType(srcNode, dstNode);
	auto& counters = s_nodeFlowTypeCounters[srcNode];
	auto& tpCounters = s_nodeTPFlowTypeCounters[srcNode][dstNode];
	if (type == FlowType::SAME_COL)
	{
		++counters.sameCol;
		++tpCounters.sameCol;
	}
	else if (type == FlowType::SAME_RACK_CROSS_COL)
	{
		++counters.sameRackCrossCol;
		++tpCounters.sameRackCrossCol;
	}
	else
	{
		++counters.crossRack;
		++tpCounters.crossRack;
	}
}

UbAlpsPacketTracker::FlowTypeCounters
UbAlpsPacketTracker::GetNodeFlowTypeCounters(uint32_t srcNode)
{
	auto it = s_nodeFlowTypeCounters.find(srcNode);
	if (it == s_nodeFlowTypeCounters.end())
	{
		return FlowTypeCounters{};
	}
	return it->second;
}

UbAlpsPacketTracker::FlowTypeCounters
UbAlpsPacketTracker::GetNodeTPFlowTypeCounters(uint32_t srcNode, uint32_t dstNode)
{
	auto nodeIt = s_nodeTPFlowTypeCounters.find(srcNode);
	if (nodeIt == s_nodeTPFlowTypeCounters.end())
	{
		return FlowTypeCounters{};
	}
	auto tpIt = nodeIt->second.find(dstNode);
	if (tpIt == nodeIt->second.end())
	{
		return FlowTypeCounters{};
	}
	return tpIt->second;
}

const std::unordered_map<uint32_t, UbAlpsPacketTracker::FlowTypeCounters>&
UbAlpsPacketTracker::GetAllNodeFlowTypeCounters()
{
	return s_nodeFlowTypeCounters;
}

DataRate
UbAlpsPacketTracker::EstimateInitialRateByType(uint32_t srcNode,
												 uint32_t dstNode,
												 DataRate maxRate)
{
	uint64_t maxRateBps = maxRate.GetBitRate();
	if (maxRateBps == 0)
	{
		maxRateBps = 400000000000ULL;
	}
    const FlowType tpType = UbAlpsPacketTracker::ClassifyFlowType(srcNode, dstNode);
	const FlowTypeCounters counters = GetNodeFlowTypeCounters(srcNode);
	const uint32_t typeFlowCount = std::max<uint32_t>(1, GetTypeFlowCount(counters, tpType));
	uint32_t tpTypeFlowCount = 1;
	const FlowTypeCounters TPcounters = GetNodeTPFlowTypeCounters(srcNode, dstNode);
	tpTypeFlowCount = std::max<uint32_t>(1, GetTypeFlowCount(TPcounters, tpType));

	const uint64_t budgetUnit = std::max<uint64_t>(1, maxRateBps / 15);
	uint64_t typeBudgetBps = 4 * budgetUnit;
	if (tpType == FlowType::SAME_COL)
	{
		typeBudgetBps = 7 * budgetUnit;
	}

	uint64_t initRateBps = std::max<uint64_t>(1, (typeBudgetBps / typeFlowCount) * tpTypeFlowCount);
	if (tpType == FlowType::SAME_COL)
	{
		initRateBps = std::max<uint64_t>(1, static_cast<uint64_t>(initRateBps * 0.4));
	}
	else
	{
		initRateBps = std::max<uint64_t>(1, (initRateBps * 6) / 10);
	}

	return DataRate(initRateBps);
}

void
UbAlpsPacketTracker::PrintNodeAllTPRates(uint32_t srcNode, DataRate maxRate)
{
	auto nodeTpIt = s_nodeTPFlowTypeCounters.find(srcNode);
	if (nodeTpIt == s_nodeTPFlowTypeCounters.end() || nodeTpIt->second.empty())
	{
		NS_LOG_UNCOND("[ALPS_INIT_CHECK][NODE=" << srcNode
											 << "] no TP flow records in s_nodeTPFlowTypeCounters");
		return;
	}

	const FlowTypeCounters nodeCounters = GetNodeFlowTypeCounters(srcNode);
	const uint32_t nodeTotalFlows = nodeCounters.sameCol + nodeCounters.sameRackCrossCol + nodeCounters.crossRack;

	NS_LOG_UNCOND("[ALPS_INIT_CHECK][NODE=" << srcNode
									 << "] nodeFlowTypeCounters(total=" << nodeTotalFlows
									 << ", same_col=" << nodeCounters.sameCol
									 << ", same_rack_cross_col=" << nodeCounters.sameRackCrossCol
									 << ", cross_rack=" << nodeCounters.crossRack << ")");

	for (const auto& tpKv : nodeTpIt->second)
	{
		const uint32_t dstNode = tpKv.first;
		const FlowTypeCounters tpCounters = tpKv.second;
		const FlowType tpType = ClassifyFlowType(srcNode, dstNode);
		const uint32_t tpTypeFlowCount = GetTypeFlowCount(tpCounters, tpType);
		const uint32_t typeFlowCount = GetTypeFlowCount(nodeCounters, tpType);
		const double ratio = (typeFlowCount == 0) ? 0.0
													  : static_cast<double>(tpTypeFlowCount) / static_cast<double>(typeFlowCount);
		const DataRate initRate = EstimateInitialRateByType(srcNode, dstNode, maxRate);

		std::ostringstream ratioOss;
		ratioOss << std::fixed << std::setprecision(4) << ratio;

		NS_LOG_UNCOND("[ALPS_INIT_CHECK][NODE=" << srcNode << "][TP(dst=" << dstNode
												 << ", type=" << GetFlowTypeName(tpType)
												 << ")] tpTypeFlowCount=" << tpTypeFlowCount
												 << " typeFlowCount=" << typeFlowCount
												 << " ratio=" << ratioOss.str()
												 << " initRateGbps="
												 << static_cast<double>(initRate.GetBitRate()) / 1e9
												 << " tpCounters(same_col=" << tpCounters.sameCol
												 << ", same_rack_cross_col=" << tpCounters.sameRackCrossCol
												 << ", cross_rack=" << tpCounters.crossRack << ")");
	}
}

void
UbAlpsPacketTracker::RecordAlpsPacketSent(uint32_t srcNode, uint32_t dstNode, uint32_t pathLength)
{
	const FlowType type = ClassifyFlowType(srcNode, dstNode);
	auto& total = GetFlowDropStatsRef(s_totalDropStats, type);
	auto& window = GetFlowDropStatsRef(s_windowDropStats, type);
	++total.sentPackets;
	++window.sentPackets;
	if (pathLength > 0)
	{
		++total.sentByPathLength[pathLength];
		++window.sentByPathLength[pathLength];
	}
	EnsureDropStatsReportScheduled();
}

void
UbAlpsPacketTracker::RecordAlpsPacketReceived(uint32_t srcNode, uint32_t dstNode)
{
	const FlowType type = ClassifyFlowType(srcNode, dstNode);
	auto& total = GetFlowDropStatsRef(s_totalDropStats, type);
	auto& window = GetFlowDropStatsRef(s_windowDropStats, type);
	++total.receivedPackets;
	++window.receivedPackets;
	EnsureDropStatsReportScheduled();
}

void
UbAlpsPacketTracker::RecordAlpsPacketDrop(uint32_t srcNode, uint32_t dstNode, uint32_t hop, uint32_t pathLength)
{
	const FlowType type = ClassifyFlowType(srcNode, dstNode);
	auto& total = GetFlowDropStatsRef(s_totalDropStats, type);
	auto& window = GetFlowDropStatsRef(s_windowDropStats, type);
	++total.droppedPackets;
	++window.droppedPackets;
	++total.dropByHop[hop];
	++window.dropByHop[hop];
	if (pathLength > 0)
	{
		++total.dropByPathLength[pathLength];
		++window.dropByPathLength[pathLength];
		++total.dropByPathLengthHop[pathLength][hop];
		++window.dropByPathLengthHop[pathLength][hop];
	}
	EnsureDropStatsReportScheduled();
}

void
UbAlpsPacketTracker::IncrementSwitchDroppedPackets()
{
	++s_totalSwitchDroppedPkts;
}

uint64_t
UbAlpsPacketTracker::GetSwitchDroppedPackets()
{
	return s_totalSwitchDroppedPkts;
}

std::string
UbAlpsPacketTracker::FormatHopDistribution(const std::map<uint32_t, uint64_t>& hopDist)
{
	if (hopDist.empty())
	{
		return "{}";
	}
	std::ostringstream oss;
	oss << "{";
	bool first = true;
	for (const auto& kv : hopDist)
	{
		if (!first)
		{
			oss << ", ";
		}
		first = false;
		oss << kv.first << "跳位置丢弃个数:" << kv.second;
	}
	oss << "}";
	return oss.str();
}

std::string
UbAlpsPacketTracker::FormatPathLengthDistribution(const std::map<uint32_t, uint64_t>& lengthDist)
{
	if (lengthDist.empty())
	{
		return "{}";
	}
	std::ostringstream oss;
	oss << "{";
	bool first = true;
	for (const auto& kv : lengthDist)
	{
		if (!first)
		{
			oss << ", ";
		}
		first = false;
		oss << kv.first << "跳路径:" << kv.second << "个数据包";
	}
	oss << "}";
	return oss.str();
}

std::string
UbAlpsPacketTracker::FormatPathLengthHopDistribution(
	const std::map<uint32_t, std::map<uint32_t, uint64_t>>& lengthHopDist)
{
	if (lengthHopDist.empty())
	{
		return "{}";
	}
	std::ostringstream oss;
	oss << "{";
	bool firstLength = true;
	for (const auto& lengthEntry : lengthHopDist)
	{
		if (!firstLength)
		{
			oss << ", ";
		}
		firstLength = false;
		oss << lengthEntry.first << "跳路径上{";
		bool firstHop = true;
		for (const auto& hopEntry : lengthEntry.second)
		{
			if (!firstHop)
			{
				oss << ", ";
			}
			firstHop = false;
			oss << hopEntry.first << "跳位置丢弃个数:" << hopEntry.second;
		}
		oss << "}";
	}
	oss << "}";
	return oss.str();
}

void
UbAlpsPacketTracker::EnsureDropStatsReportScheduled()
{
	if (s_dropStatsReportScheduled==false)
	{
		return;
	}
	s_lastDropStatsReportTime = Simulator::Now();
	Simulator::Schedule(MicroSeconds(10), &UbAlpsPacketTracker::ReportDropStatsPeriodic);
	s_dropStatsReportScheduled = false;
}

void
UbAlpsPacketTracker::ReportDropStatsPeriodic()
{
	const Time now = Simulator::Now();
	const int64_t windowUs = (now - s_lastDropStatsReportTime).GetMicroSeconds();

	const FlowDropStats* totals[3] = {
		&s_totalDropStats.sameCol,
		&s_totalDropStats.sameRackCrossCol,
		&s_totalDropStats.crossRack,
	};
	const FlowDropStats* windows[3] = {
		&s_windowDropStats.sameCol,
		&s_windowDropStats.sameRackCrossCol,
		&s_windowDropStats.crossRack,
	};
	const FlowType types[3] = {
		FlowType::SAME_COL,
		FlowType::SAME_RACK_CROSS_COL,
		FlowType::CROSS_RACK,
	};

	uint64_t totalSentAll = 0;
	uint64_t totalDropAll = 0;
	uint64_t windowSentAll = 0;
	uint64_t windowDropAll = 0;
	std::map<uint32_t, uint64_t> totalHopAll;
	std::map<uint32_t, uint64_t> windowHopAll;

	for (int i = 0; i < 3; ++i)
	{
		totalSentAll += totals[i]->sentPackets;
		totalDropAll += totals[i]->droppedPackets;
		windowSentAll += windows[i]->sentPackets;
		windowDropAll += windows[i]->droppedPackets;
		for (const auto& kv : totals[i]->dropByHop)
		{
			totalHopAll[kv.first] += kv.second;
		}
		for (const auto& kv : windows[i]->dropByHop)
		{
			windowHopAll[kv.first] += kv.second;
		}
	}

	const double totalDropRatio =
		(totalSentAll == 0) ? 0.0 : static_cast<double>(totalDropAll) / static_cast<double>(totalSentAll);
	const double windowDropRatio = (windowSentAll == 0)
									   ? 0.0
									   : static_cast<double>(windowDropAll) / static_cast<double>(windowSentAll);
	NS_LOG_UNCOND("=============================================================================");
	NS_LOG_UNCOND("[ALPS_DROP_STATS] t="
				  << now.GetMicroSeconds() << "us ,丢包数（s_totalSwitchDropedPkts）： "
				  << s_totalSwitchDroppedPkts<<" 总重传数："<<UbTransportChannel::s_totalActiveRetransSent+UbTransportChannel::s_totaltimeoutretrans
				  << " \n总发包数：" << totalSentAll << " 总丢包数：" << totalDropAll
				  << " 丢包率：" << totalDropRatio << " 丢包分布：" << FormatHopDistribution(totalHopAll) << "\n"
				  << " " << windowUs << "us内丢包数：" << windowDropAll << " 丢包率：" << windowDropRatio
				  << " 丢包分布：" << FormatHopDistribution(windowHopAll) << "\n");
	NS_LOG_UNCOND("------------------------------------------------------------------------------");
	for (int i = 0; i < 3; ++i)
	{
		const uint64_t typeInFlight =
			(totals[i]->sentPackets >= totals[i]->receivedPackets + totals[i]->droppedPackets)
				? totals[i]->sentPackets - totals[i]->receivedPackets - totals[i]->droppedPackets
				: 0;
		const double typeTotalRatio = (totals[i]->sentPackets == 0)
										  ? 0.0
										  : static_cast<double>(totals[i]->droppedPackets) /
												static_cast<double>(totals[i]->sentPackets);
		const double typeWindowRatio = (windows[i]->sentPackets == 0)
										   ? 0.0
										   : static_cast<double>(windows[i]->droppedPackets) /
												 static_cast<double>(windows[i]->sentPackets);

		NS_LOG_UNCOND("[" << GetFlowTypeName(types[i]) << "] \n总发包数：" << totals[i]->sentPackets
						  << " 总在途包数：" << typeInFlight
						  << " 总接收包数：" << totals[i]->receivedPackets
						  << " 总丢包数：" << totals[i]->droppedPackets << " 丢包率：" << typeTotalRatio
						  << " 丢包分布：" << FormatHopDistribution(totals[i]->dropByHop) << "\n"
						  << " " << windowUs << "us内发包数：" << windows[i]->sentPackets
						  << " 接收包数：" << windows[i]->receivedPackets
						  << " 丢包数：" << windows[i]->droppedPackets << " 丢包率：" << typeWindowRatio
						  << " 丢包分布：" << FormatHopDistribution(windows[i]->dropByHop));
		NS_LOG_UNCOND("------------------------------------------------------------------------------");
		if (types[i] == FlowType::CROSS_RACK)
		{
			NS_LOG_UNCOND("[cross_rack 按照路径长度分类] \n 总发包分布："
						  << FormatPathLengthDistribution(totals[i]->sentByPathLength) << "\n"
						  << " 总丢包分布：" << FormatPathLengthDistribution(totals[i]->dropByPathLength)
						  << "\n"
						  << " 长度-丢包位置分布："
						  << FormatPathLengthHopDistribution(totals[i]->dropByPathLengthHop) << "\n"
						  << "---" << windowUs << "us内发包分布："
						  << FormatPathLengthDistribution(windows[i]->sentByPathLength) << "\n"
						  << "---丢包分布：" << FormatPathLengthDistribution(windows[i]->dropByPathLength)
						  << "\n"
						  << "---长度-丢包位置分布："
						  << FormatPathLengthHopDistribution(windows[i]->dropByPathLengthHop));
		}
	}
	NS_LOG_UNCOND("=============================================================================");
	s_windowDropStats = GlobalDropStats{};
	s_lastDropStatsReportTime = now;
	Simulator::Schedule(MicroSeconds(10), &UbAlpsPacketTracker::ReportDropStatsPeriodic);
}

uint32_t
UbAlpsPacketTracker::GetTypeFlowCount(const FlowTypeCounters& counters, FlowType type)
{
	if (type == FlowType::SAME_COL)
	{
		return counters.sameCol;
	}
	if (type == FlowType::SAME_RACK_CROSS_COL)
	{
		return counters.sameRackCrossCol;
	}
	return counters.crossRack;
}

void
UbAlpsNodeReceiveTracker::Reset()
{
	s_nodeReceiveCounters.clear();
	s_reportScheduled = false;
}

void
UbAlpsNodeReceiveTracker::EnsureReportScheduled()
{
	if (s_reportScheduled)
	{
		return;
	}
	s_reportScheduled = true;
	Simulator::ScheduleDestroy(&UbAlpsNodeReceiveTracker::ReportAtSimulationEnd);
}

void
UbAlpsNodeReceiveTracker::RecordPacketReceived(uint32_t nodeId, uint32_t packetSizeBytes,uint32_t Taskid)
{
	if (!s_enableNodeReceiveTracker)
	{
		return;
	}

	auto& counters = s_nodeReceiveCounters[nodeId];
	if (packetSizeBytes >= 4000)
	{
		++counters.ge4000Bytes;
		counters.TaskisCompleted[Taskid]++;
	}
	else
	{
		++counters.lt4000Bytes;
		counters.TaskisCompleted[Taskid]++;
	}

	EnsureReportScheduled();
}

void
UbAlpsNodeReceiveTracker::ReportAtSimulationEnd()
{
	if (!s_enableNodeReceiveTracker)
	{
		return;
	}

	const std::string outputPath =
		"/app/ns-3-ub/scratch/tp_UB_Mesh_V0002/ub_alps_node_receive.log";
	std::ofstream out(outputPath, std::ios::out | std::ios::trunc);
	if (!out.is_open())
	{
		NS_LOG_UNCOND("[ALPS_NODE_RECV_TRACKER] failed to open output file: " << outputPath);
		return;
	}

	out << "nodeid,4000+B,4000-B\n";
	std::vector<uint32_t> nodeIds;
	nodeIds.reserve(s_nodeReceiveCounters.size());
	for (const auto& kv : s_nodeReceiveCounters)
	{
		nodeIds.push_back(kv.first);
	}
	std::sort(nodeIds.begin(), nodeIds.end());

	for (uint32_t nodeId : nodeIds)
	{
		const auto& c = s_nodeReceiveCounters[nodeId];
		out << nodeId << "," << c.ge4000Bytes << "," << c.lt4000Bytes << "。";
		for (auto i:c.TaskisCompleted)
		{    if(i.second==-1){
			out << "[Taskid:"<<i.first<<" , 未开始 ];";
		}else if(i.second==0){
			out << "[Taskid:"<<i.first<<" , 进行中 ];";
		}
		
		}
		out << "\n";
	}
	out.close();

	NS_LOG_UNCOND("[ALPS_NODE_RECV_TRACKER] report written to " << outputPath);
}
void UbAlpsNodeReceiveTracker::RecordTask(uint32_t dstnodeId, uint32_t Taskid){
	if (!s_enableNodeReceiveTracker)
	{
		return;
	}
	 s_nodeReceiveCounters[dstnodeId].TaskisCompleted[Taskid]=-1;
}
} // namespace ns3
