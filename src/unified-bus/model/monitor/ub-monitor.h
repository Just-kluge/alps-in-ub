// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_MONITOR_H
#define UB_MONITOR_H

#include <cstdint>
#include <fstream>
#include <ostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

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
	static void InitializeFeatureSwitchesFromConfig();
	static FlowType ClassifyFlowType(uint32_t srcNode, uint32_t dstNode);
	/**
	 * @brief 记录一条“计划发送流”到全局统计中。
	 *
	 * 该接口会同时维护两层计数：
	 * 1) 节点级计数：s_nodeFlowTypeCounters[srcNode]
	 *    - 表示 srcNode 在每种 FlowType 上的总流数量。
	 * 2) TP级计数：s_nodeTPFlowTypeCounters[srcNode][dstNode]
	 *    - 表示 srcNode 到指定 TP(当前以 dstNode 作为键)在每种 FlowType 上的流数量。
	 *
	 * 设计目的：
	 * - 节点级计数用于得到某 FlowType 的总池子大小(typeFlowCount)。
	 * - TP级计数用于得到该 TP 在该 FlowType 内的占比(tpTypeFlowCount/typeFlowCount)。
	 * - 后续初始化速率可按该占比分配。
	 *
	 * @param srcNode 源节点 ID。
	 * @param dstNode 目的节点 ID。当前实现中用作 TP 维度的索引键。
	 * @param Taskid 任务 ID。奇数任务被视为 combine 阶段，不参与初始化流数量统计。
	 */
	static void RecordPlannedFlow(uint32_t srcNode, uint32_t dstNode,uint32_t Taskid);
	static FlowTypeCounters GetNodeFlowTypeCounters(uint32_t srcNode);
	static FlowTypeCounters GetNodeTPFlowTypeCounters(uint32_t srcNode, uint32_t dstNode);
	static const std::unordered_map<uint32_t, FlowTypeCounters>& GetAllNodeFlowTypeCounters();
	/**
	 * @brief 按“TP在该类型内的流数量占比”估算初始发送速率。
	 *
	 * 计算步骤：
	 * 1) 使用 ClassifyFlowType(srcNode, dstNode) 得到该 TP 的流类型 tpType。
	 * 2) 取该源节点在 tpType 上的总流数 typeFlowCount。
	 * 3) 取该 TP(以 dstNode 索引)在 tpType 上的流数 tpTypeFlowCount。
	 * 4) 先为每种类型分配预算 typeBudgetBps：
	 *    - SAME_COL: 7/15 * maxRate
	 *    - 其它类型: 4/15 * maxRate
	 * 5) 再按占比分配给当前 TP：
	 *    initRateBps = max(1, (typeBudgetBps / typeFlowCount) * tpTypeFlowCount)
	 * 6) 保留现有类型缩放策略：
	 *    - SAME_COL: 乘 0.5
	 *    - 其它类型: 乘 0.8
	 *
	 * 当统计缺失时，计数会通过 max(1, ...) 兜底，避免除零并保证最小 1bps 输出。
	 *
	 * @param srcNode 源节点 ID。
	 * @param dstNode 目的节点 ID。当前实现中用作 TP 维度的索引键。
	 * @param maxRate 源端口最大速率；若为 0，内部回退到 400Gbps。
	 * @return DataRate 估算得到的 TP 初始发送速率。
	 */
	static DataRate EstimateInitialRateByType(uint32_t srcNode,
									  uint32_t dstNode,
									  DataRate maxRate);

	static void RecordAlpsPacketSent(uint32_t srcNode, uint32_t dstNode, uint32_t pathLength);
	static void RecordAlpsPacketReceived(uint32_t srcNode, uint32_t dstNode);
	static void RecordAlpsPacketDrop(uint32_t srcNode, uint32_t dstNode, uint32_t hop, uint32_t pathLength);
    static void PrintNodeAllTPRates(uint32_t srcNode, DataRate maxRate);
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
	static std::unordered_map<uint32_t, std::unordered_map<uint32_t, FlowTypeCounters>>
		s_nodeTPFlowTypeCounters;
	static GlobalDropStats s_totalDropStats;
	static GlobalDropStats s_windowDropStats;
	//输出速率初始化用的哪些流，仅仅输出一次
    static bool s_shouldPrintlog;
	static bool s_dropStatsReportScheduled;
	static Time s_lastDropStatsReportTime;
	static uint64_t s_totalSwitchDroppedPkts;
};

class UbAlpsNodeReceiveTracker
{//该类用来监测节点的接收情况，分析是否匹配节点流量生成概率，仅适用于despatch阶段
 //最终目的是解决部分task异常结束的问题	（已解决）
  public:
	struct NodeReceiveCounters
	{
		uint64_t ge4000Bytes = 0;
		uint64_t lt4000Bytes = 0;
		//键为taskid,值为任务是否完成，-1为未开始，0为进行中，1为完成
		std::unordered_map<uint32_t, int> TaskisCompleted;
	};

	// Global switch for enabling/disabling node receive tracking.
	static bool s_enableNodeReceiveTracker;

	static void Reset();
	static void RecordPacketReceived(uint32_t nodeId, uint32_t packetSizeBytes,uint32_t Taskid);
	static void ReportAtSimulationEnd();
    static void RecordTask(uint32_t dstnodeId, uint32_t Taskid);
  private:
	static void EnsureReportScheduled();

	static std::unordered_map<uint32_t, NodeReceiveCounters> s_nodeReceiveCounters;
	static bool s_reportScheduled;
};

class UbPortMetricsSampler
{
  public:
	static void Start(const std::string& outputDir);
	static void Stop();
	static bool IsRunning();

  private:
	struct SamplerState
	{
		Time interval;
		std::string label;
		std::string outputFile;
		std::ofstream stream;
		std::unordered_map<uint64_t, uint64_t> lastTxBytes;
	};

	static void EnsureOutputDirectory(const std::string& outputDir);
	static void OpenSampler(SamplerState& sampler, const std::string& outputDir);
	static void ScheduleNext(const std::string& label);
	static void Sample(const std::string& label);
	static std::string BuildOutputPath(const std::string& outputDir, const std::string& label);
	static uint64_t MakePortKey(uint32_t nodeId, uint32_t portId);
	static std::string FormatSeconds(double value);

	static std::unordered_map<std::string, SamplerState> s_samplers;
	static std::string s_outputDir;
	static bool s_running;
};

class UbTaskFctMonitor
{
  public:
	struct TaskFctRecord
	{
		uint32_t nodeId = 0;
		uint32_t taskId = 0;
		int64_t dstNodeId = -1;
		int64_t startNs = -1;
		int64_t endNs = -1;
		int64_t fctNs = -1;
		std::string status;
	};

	static void Start(const std::string& outputDir);
	static void Stop();
	static bool IsRunning();
	static void RecordTaskDestination(uint32_t taskId, uint32_t dstNodeId);
	static void RecordTaskStart(uint32_t nodeId, uint32_t taskId);
	static void RecordTaskComplete(uint32_t nodeId, uint32_t taskId);

  private:
	static void EnsureOutputDirectory(const std::string& outputDir);
	static std::string BuildOutputPath(const std::string& outputDir);
	static uint64_t MakeTaskKey(uint32_t nodeId, uint32_t taskId);
 	static int64_t ComputePercentileFct(const std::vector<TaskFctRecord>& records, double percentile);

	static std::unordered_map<uint64_t, Time> s_taskStartTimes;
	static std::unordered_map<uint32_t, uint32_t> s_taskDestNodes;
	static std::vector<TaskFctRecord> s_completedRecords;
	static std::vector<TaskFctRecord> s_anomalyRecords;
	static std::string s_outputFile;
	static bool s_running;
};

} // namespace ns3

#endif // UB_MONITOR_H
