// SPDX-License-Identifier: GPL-2.0-only
#ifndef UTILS_H
#define UTILS_H

#include <iostream>
#include <sstream>
#include <chrono>
#include <map>
#include <fstream>
#include <tuple>
#include "ns3/core-module.h"
#include "ns3/singleton.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ptr.h"
#include "ns3/object-factory.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-traffic-gen.h"
#include "ns3/ub-app.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/config-store.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-network-address.h"
#include "ub-tp-connection-manager.h"
#include "ns3/random-variable-stream.h"
#include "ns3/enum.h"
#include "ns3/ub-fault.h"

namespace utils {
/**
 *  @brief UbUtils单例类
 */
class UbUtils : public ns3::Singleton<UbUtils> {
public:
    // 保存node
    inline static std::string trace_path;

    inline static std::map<std::string, std::ofstream *> files;  // 存储文件名和对应的文件句柄

    ns3::GlobalValue g_fault_enable =
    ns3::GlobalValue("UB_FAULT_ENABLE", "fault moudle enabled", ns3::BooleanValue(false), ns3::MakeBooleanChecker());

    void PrintTimestamp(const std::string &message);

    void ParseTrace(bool isTest = false);

    void Destroy();

    void CreateTraceDir();

    // 创建node
    void CreateNode(const std::string &filename);

    std::vector<TrafficRecord> ReadTrafficCSV(const std::string &filename);

    // 读取拓扑文件
    void CreateTopo(const std::string &filename);

    // 读取路由
    void AddRoutingTable(const std::string &filename);
    void AddRoutingTableAlps(const std::string &filenamePST, const std::string &filenamePIT);

    void CreateTp(const std::string &filename);

    // 从TXT文件加载配置
    void SetComponentsAttribute(const std::string &filename);

    void TopoTraceConnect();

    // 为单个TP和Trace回调函数链接
    void SingleTpTraceConnect(uint32_t nodeId, uint32_t tpn);

    void ClientTraceConnect(int srcNode);

    bool QueryAttributeInfo(int argc, char *argv[]);

    void InitFaultMoudle(const std::string &FaultConfigFile);

private:
    // 读取Traffic配置文件
    enum class FIELDCOUNT : int {
       TASKID = 0,
       SOURCENODE = 1,
       DESTNODE = 2,
       DATASIZE,
       OPTYPE,
       PRIORITY,
       DELAY,
       PHASEID,
       DEPENDONPHASES
    };

    struct NodeEle {
        std::string nodeIdStr;

        std::string nodeTypeStr;

        std::string portNumStr;

        std::string forwardDelay;
    };

    std::map<uint32_t, NodeEle> nodeEle_map;

    std::string g_config_path;

    bool TraceEnable = false;
    bool TaskTraceEnable = true;
    bool PacketTraceEnable = true;
    bool PortTraceEnable = true;
    bool RecordTraceEnabled = false;

    bool isTest = false;

    // 设置Trace全局变量
    ns3::GlobalValue g_trace_enable = ns3::GlobalValue("UB_TRACE_ENABLE",
                                                       "Master switch for all traces",
                                                       ns3::BooleanValue(false),
                                                       ns3::MakeBooleanChecker());

    ns3::GlobalValue g_task_trace_enable = ns3::GlobalValue("UB_TASK_TRACE_ENABLE",
                                                            "Enable task and WQE level traces",
                                                            ns3::BooleanValue(true),
                                                            ns3::MakeBooleanChecker());

    ns3::GlobalValue g_packet_trace_enable = ns3::GlobalValue("UB_PACKET_TRACE_ENABLE",
                                                              "Enable packet send/ack/receive traces",
                                                              ns3::BooleanValue(true),
                                                              ns3::MakeBooleanChecker());

    ns3::GlobalValue g_port_trace_enable = ns3::GlobalValue("UB_PORT_TRACE_ENABLE",
                                                            "Enable port-level Tx/Rx traces (very noisy)",
                                                            ns3::BooleanValue(true),
                                                            ns3::MakeBooleanChecker());

    ns3::GlobalValue g_parse_enable = ns3::GlobalValue("UB_PARSE_TRACE_ENABLE",
                                                       "enable parse trace",
                                                       ns3::BooleanValue(false),
                                                       ns3::MakeBooleanChecker());

    ns3::GlobalValue g_record_pkt_trace_enable = ns3::GlobalValue("UB_RECORD_PKT_TRACE",
                                                                  "enable record all packet trace",
                                                                  ns3::BooleanValue(false),
                                                                  ns3::MakeBooleanChecker());

    ns3::GlobalValue g_python_script_path =
    ns3::GlobalValue("UB_PYTHON_SCRIPT_PATH",
                     "Path to parse_trace.py script (REQUIRED - must be set by user)",
                     ns3::StringValue("/path/to/ns-3-ub-tools/trace_analysis/parse_trace.py"),
                     ns3::MakeStringChecker());

    static std::string Among(std::string s, std::string ts);

    void SetRecord(int fieldCount, std::string field, TrafficRecord &record);

    static void PrintTraceInfo(std::string fileName, std::string info);

    static void PrintTraceInfoNoTs(std::string fileName, std::string info);

    static void TpFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                         uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort);

    static void TpLastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                        uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort);

    static void TpLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                       uint32_t tpMsn, uint32_t psn, uint32_t sPort);

    static void TpLastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                           uint32_t tpMsn, uint32_t psn, uint32_t dPort);

    static void TpWqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);

    static void TpWqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);

    static void TpRecvNotify(uint32_t packetUid, uint32_t psn, uint32_t src, uint32_t dst, uint32_t srcTpn,
                             uint32_t dstTpn, ns3::PacketType type, uint32_t size, uint32_t taskId,
                             ns3::UbPacketTraceTag traceTag);

    static void LdstRecvNotify(uint32_t packetUid,
                               uint32_t src,
                               uint32_t dst,
                               ns3::PacketType type,
                               uint32_t size,
                               uint32_t taskId,
                               ns3::UbPacketTraceTag traceTag);

    static void LdstFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagMemTaskStartsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void DagWqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    static void DagWqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    static void PortTxNotify(uint32_t nodeId, uint32_t portId, uint32_t size);

    static void PortRxNotify(uint32_t nodeId, uint32_t portId, uint32_t size);

    static void LdstThreadMemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void LdstThreadFirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstThreadLastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId);

    static void LdstPeerSendFirstPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t type);

    static void SwitchLastPacketTraversesNotify(uint32_t nodeId, ns3::UbTransportHeader ubTpHeader);

    // 解析节点范围（如 "1..4"）
    inline void ParseNodeRange(const std::string &rangeStr, NodeEle nodeEle);

    // 读取TP配置文件
    void ParseLine(const std::string &line, Connection &conn);
};

}  // namespace utils

#endif  // UTILS_H
