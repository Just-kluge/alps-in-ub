// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-alps-pst.h"
#include "ns3/ub-routing-process.h"  // 这里包含完整定义

namespace ns3 {

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