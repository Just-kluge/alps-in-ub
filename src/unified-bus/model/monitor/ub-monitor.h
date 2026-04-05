// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_MONITOR_H
#define UB_MONITOR_H

#include <cstdint>
#include <ostream>
#include <map>
#include <string>
#include <unordered_map>

#include "ns3/data-rate.h"
#include "ns3/nstime.h"

namespace ns3 {

class UbAlpsPacketTracker
{
  public:
	enum class FlowType : uint8_t
	{
		SAME_COL = 0,
		SAME_RACK_CROSS_COL = 1,
		CROSS_RACK = 2,
	};

	struct FlowTypeCounters
	{
		uint32_t sameCol = 0;
		uint32_t sameRackCrossCol = 0;
		uint32_t crossRack = 0;
	};

	struct FlowDropStats
	{
		uint64_t sentPackets = 0;
		uint64_t receivedPackets = 0;
		uint64_t droppedPackets = 0;
		std::map<uint32_t, uint64_t> dropByHop;
		std::map<uint32_t, uint64_t> sentByPathLength;
		std::map<uint32_t, uint64_t> dropByPathLength;
		std::map<uint32_t, std::map<uint32_t, uint64_t>> dropByPathLengthHop;
	};

	struct GlobalDropStats
	{
		FlowDropStats sameCol;
		FlowDropStats sameRackCrossCol;
		FlowDropStats crossRack;
	};

	static void ResetGlobalFlowTypeStats();
	static FlowType ClassifyFlowType(uint32_t srcNode, uint32_t dstNode);
	static void RecordPlannedFlow(uint32_t srcNode, uint32_t dstNode);
	static FlowTypeCounters GetNodeFlowTypeCounters(uint32_t srcNode);
	static const std::unordered_map<uint32_t, FlowTypeCounters>& GetAllNodeFlowTypeCounters();
	static DataRate EstimateInitialRateByType(uint32_t srcNode, FlowType type, DataRate maxRate);

	static void RecordAlpsPacketSent(uint32_t srcNode, uint32_t dstNode, uint32_t pathLength);
	static void RecordAlpsPacketReceived(uint32_t srcNode, uint32_t dstNode);
	static void RecordAlpsPacketDrop(uint32_t srcNode, uint32_t dstNode, uint32_t hop, uint32_t pathLength);

	static void IncrementSwitchDroppedPackets();
	static uint64_t GetSwitchDroppedPackets();

  private:
	static uint32_t GetTypeFlowCount(const FlowTypeCounters& counters, FlowType type);
	static void EnsureDropStatsReportScheduled();
	static void ReportDropStatsPeriodic();
	static std::string FormatHopDistribution(const std::map<uint32_t, uint64_t>& hopDist);
	static std::string FormatPathLengthDistribution(const std::map<uint32_t, uint64_t>& lengthDist);
	static std::string FormatPathLengthHopDistribution(
		const std::map<uint32_t, std::map<uint32_t, uint64_t>>& lengthHopDist);

	static std::unordered_map<uint32_t, FlowTypeCounters> s_nodeFlowTypeCounters;
	static GlobalDropStats s_totalDropStats;
	static GlobalDropStats s_windowDropStats;
	static bool s_dropStatsReportScheduled;
	static Time s_lastDropStatsReportTime;
	static uint64_t s_totalSwitchDroppedPkts;
};

} // namespace ns3

#endif // UB_MONITOR_H
