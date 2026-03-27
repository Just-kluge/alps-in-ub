// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-alps.h"
#include "ns3/boolean.h"
#include "ns3/enum.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transport.h"
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbCongestionControl");
NS_OBJECT_ENSURE_REGISTERED(UbCongestionControl);

std::ostream& operator<<(std::ostream& os, CongestionCtrlAlgo algo) {
    switch (algo) {
        case CAQM:  return os << "CAQM";
        case ALPS:  return os << "ALPS";
        case LDCP:  return os << "LDCP";
        case DCQCN: return os << "DCQCN";
        default:    return os << "Unknown";
    }
}

GlobalValue g_congestionCtrlAlgo =
    GlobalValue("UB_CC_ALGO",
                "Congestion control algorithm",
                EnumValue(CongestionCtrlAlgo::CAQM),
                MakeEnumChecker (CongestionCtrlAlgo::CAQM, "CAQM",
                                 CongestionCtrlAlgo::ALPS, "ALPS",
                                 CongestionCtrlAlgo::LDCP, "LDCP",
                                 CongestionCtrlAlgo::DCQCN, "DCQCN"));

GlobalValue g_congestionCtrlEnabled =
    GlobalValue("UB_CC_ENABLED",
                "Congestion control enabled",
                BooleanValue(false),
                MakeBooleanChecker());

TypeId UbCongestionControl::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbCongestionControl")
            .SetParent<ns3::Object>()
            .AddConstructor<UbCongestionControl>();
    return tid;
}

UbCongestionControl::UbCongestionControl()
{
    BooleanValue ccEnabledval;
    g_congestionCtrlEnabled.GetValue(ccEnabledval); // 从全局值中，获取是否启用拥塞控制
    m_congestionCtrlEnabled = ccEnabledval.Get();   // 设置成员变量 
    EnumValue<CongestionCtrlAlgo> algoValue;
    g_congestionCtrlAlgo.GetValue(algoValue); // 从全局值中，获取拥塞控制算法的具体类型
    m_algoType = algoValue.Get(); // 设置成员变量
    NS_LOG_DEBUG("enabled: " << m_congestionCtrlEnabled << " algo:" << m_algoType);
}

UbCongestionControl::~UbCongestionControl()
{
}

Ptr<UbCongestionControl> UbCongestionControl::Create(UbNodeType_t nodeType)
{
    EnumValue<CongestionCtrlAlgo> val;
    g_congestionCtrlAlgo.GetValue(val);
    CongestionCtrlAlgo algo = val.Get();
    if (algo == CAQM && nodeType == UB_DEVICE) {
        return CreateObject<UbHostCaqm>();
    } else if (algo == CAQM && nodeType == UB_SWITCH) {
        return CreateObject<UbSwitchCaqm>();
    } else {
        // Other congestion control algorithms to be extended
        // ALPS algo:
        if (algo == ALPS && nodeType == UB_DEVICE) {
            return CreateObject<UbHostAlps>();
        } else if (algo == ALPS && nodeType == UB_SWITCH) {
            return CreateObject<UbSwitchAlps>();
        }
        return nullptr;
    }
}
}
