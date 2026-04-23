// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-alps-pst.h"
#include "ns3/ub-routing-process.h"  // 这里包含完整定义
#include "ns3/global-value.h"
#include "ns3/boolean.h"
#include "ns3/log.h"
namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbAlpsPst");

GlobalValue g_alpsEnableVirtualLatency(
    "UB_ALPS_ENABLE_VIRTUAL_LATENCY",
    "Enable virtual latency in AlpsPitEntry for load balancing.",
    BooleanValue(false),
    MakeBooleanChecker());

GlobalValue g_alpsEnablePathWeight(
    "UB_ALPS_ENABLE_PATH_WEIGHT",
    "Enable path weight in AlpsPitEntry weight calculation.",
    BooleanValue(false),
    MakeBooleanChecker());

GlobalValue g_alpsEnablePerPathBdpLimit(
    "UB_ALPS_ENABLE_PER_PATH_BDP_LIMIT",
    "Enable per-path BDP-like in-flight limit in AlpsPitEntry.",
    BooleanValue(false),
    MakeBooleanChecker());

bool AlpsPitEntry::s_enableVirtualLatency = false;// 默认不启用虚时延
bool AlpsPitEntry::s_enablePathWeight= false; // 默认不启用路径权重
bool AlpsPitEntry::s_enablePerPathBdpLimit = false; // 默认不启用Per Path BDP

void AlpsPitEntry::InitializeFeatureSwitchesFromConfig()
{
    BooleanValue virtualLatencyValue(false);
    if (GlobalValue::GetValueByNameFailSafe("UB_ALPS_ENABLE_VIRTUAL_LATENCY", virtualLatencyValue)) {
        s_enableVirtualLatency = virtualLatencyValue.Get();
    }

    BooleanValue pathWeightValue(false);
    if (GlobalValue::GetValueByNameFailSafe("UB_ALPS_ENABLE_PATH_WEIGHT", pathWeightValue)) {
        s_enablePathWeight = pathWeightValue.Get();
    }

    BooleanValue perPathBdpValue(false);
    if (GlobalValue::GetValueByNameFailSafe("UB_ALPS_ENABLE_PER_PATH_BDP_LIMIT", perPathBdpValue)) {
        s_enablePerPathBdpLimit = perPathBdpValue.Get();
    }

    NS_LOG_UNCOND("[ALPS_FEATURE_SWITCH] s_enableVirtualLatency="
                  << (s_enableVirtualLatency ? "true" : "false")
                  << ", s_enablePathWeight="
                  << (s_enablePathWeight ? "true" : "false")
                  << ", s_enablePerPathBdpLimit="
                  << (s_enablePerPathBdpLimit ? "true" : "false"));
}
uint32_t AlpsPitEntry::GetRealTimeLatency( Ptr<UbRoutingProcess>  ubRoutingProcess)const 
{
    uint32_t RealTimeLatency = noQueueLatencyNs;
    //std::cout << "noQueueLatencyNs: " << noQueueLatencyNs << std::endl;
    //从 list 里面累加每一跳的排队时延，得到实时的路径时延
    for(uint32_t i = 0; i < ports.size(); i++) {
        if (ubRoutingProcess != nullptr) {
            RealTimeLatency += ubRoutingProcess->GetNodePortQueueDelay(nodes[i], ports[i]);
       //std::cout << "Node: " << nodes[i] << ", Port: " << ports[i] << ", queueLatency: " << ubRoutingProcess->GetNodePortQueueDelay(nodes[i], ports[i]) << std::endl;
        }else{
          std::cout << "ubRoutingProcess is nullptr" << std::endl;
        }
    }
   // std::cout << "RealTimeLatency: " << RealTimeLatency << std::endl;
   // std::cout<<"======================================"<<std::endl;
    return RealTimeLatency;
}

} // namespace ns3