// SPDX-License-Identifier: GPL-2.0-only

#include "ns3/ub-monitor.h"

#include <algorithm>
#include <sstream>
#include "ns3/ub-transport.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbMonitor");

std::unordered_map<uint32_t, UbAlpsPacketTracker::FlowTypeCounters>
	UbAlpsPacketTracker::s_nodeFlowTypeCounters;
UbAlpsPacketTracker::GlobalDropStats UbAlpsPacketTracker::s_totalDropStats;
UbAlpsPacketTracker::GlobalDropStats UbAlpsPacketTracker::s_windowDropStats;
//===========true表示输出数据包全局信息打印到控制台中；=======================
bool UbAlpsPacketTracker::s_dropStatsReportScheduled = true;
Time UbAlpsPacketTracker::s_lastDropStatsReportTime = Seconds(0);
uint64_t UbAlpsPacketTracker::s_totalSwitchDroppedPkts = 0;

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
UbAlpsPacketTracker::RecordPlannedFlow(uint32_t srcNode, uint32_t dstNode)
{
	const FlowType type = ClassifyFlowType(srcNode, dstNode);
	auto& counters = s_nodeFlowTypeCounters[srcNode];
	if (type == FlowType::SAME_COL)
	{
		++counters.sameCol;
	}
	else if (type == FlowType::SAME_RACK_CROSS_COL)
	{
		++counters.sameRackCrossCol;
	}
	else
	{
		++counters.crossRack;
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

const std::unordered_map<uint32_t, UbAlpsPacketTracker::FlowTypeCounters>&
UbAlpsPacketTracker::GetAllNodeFlowTypeCounters()
{
	return s_nodeFlowTypeCounters;
}

DataRate
UbAlpsPacketTracker::EstimateInitialRateByType(uint32_t srcNode, FlowType type, DataRate maxRate)
{
	uint64_t maxRateBps = maxRate.GetBitRate();
	if (maxRateBps == 0)
	{
		maxRateBps = 400000000000ULL;
	}

	const FlowTypeCounters counters = GetNodeFlowTypeCounters(srcNode);
	const uint32_t typeFlowCount = std::max<uint32_t>(1, GetTypeFlowCount(counters, type));

	const uint64_t budgetUnit = std::max<uint64_t>(1, maxRateBps / 15);
	uint64_t typeBudgetBps = 4 * budgetUnit;
	if (type == FlowType::SAME_COL)
	{
		typeBudgetBps = 7 * budgetUnit;
	}

	uint64_t initRateBps = std::max<uint64_t>(1, typeBudgetBps / typeFlowCount);
	if (type == FlowType::SAME_COL)
	{
		initRateBps = std::max<uint64_t>(1, static_cast<uint64_t>(initRateBps * 0.5));
	}
	else
	{
		initRateBps = std::max<uint64_t>(1, (initRateBps * 8) / 10);
	}

	return DataRate(initRateBps);
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

} // namespace ns3
