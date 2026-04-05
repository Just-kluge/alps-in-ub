import os
import sys
from pathlib import Path
import networkx as nx

# 允许在 ALPS_tools/ 子目录直接运行本脚本时，导入上一级的 net_sim_builder.py
_THIS_DIR = Path(__file__).resolve().parent
_PARENT_DIR = _THIS_DIR.parent
if str(_PARENT_DIR) not in sys.path:
    sys.path.insert(0, str(_PARENT_DIR))

import net_sim_builder as netsim

# =========================
# Topology macros
# =========================

# 该拓扑的层次可理解为：
# Host -> ToR(机架接入交换) -> LRS(每层包含 DU + L1) -> HRS(更高层汇聚)
#
# 当前参数下：
# - 1 个 POD
# - 4 个 Rack
# - 每个 Rack 8x8 = 64 个 Host，同时对应 64 个 ToR 位置（每个位置一个 ToR）
# - 每个 Rack 有 4 层 LRS；每层 16 个 DU + 4 个 L1
# - 全网共享 4 个 HRS（按 layer 绑定：layer i -> HRS[i]）
#
# 可用于快速 sanity check 的节点总数（当前参数）：
# hosts = 1*4*8*8 = 256
# tors  = 1*4*8*8 = 256
# dus   = 1*4*4*16 = 256
# l1s   = 1*4*4*4 = 64
# hrs   = 4
# total = 836

# POD 数量（当前测试用 1）
NUM_POD = 1

# Rack 数量（当前 256 NPU = 4 Rack）
NUM_RACK = 4

# 每个 Rack 内部结构
NUM_ROWS = 8 # 行数
NUM_COLS = 8 # 列数
NUM_HOSTS_PER_RACK = NUM_ROWS * NUM_COLS # 每个 Rack 内 Host 数量（每个位置一个 Host）, 第1列为1~8，第2列为9~16，以此类推
NUM_TORS_PER_RACK = NUM_ROWS * NUM_COLS

# 每个 LRS 的层数
# 新 LRS = 4 层，每层 = 旧 LRS
NUM_LAYERS = 4

# 每个 LRS layer 的结构
NUM_DU_PER_LRS = 16
NUM_L1_PER_LRS = 4

# HRS 数量（与 LRS_LAYERS 一一对应）
NUM_HRS = 4

if NUM_HRS != NUM_LAYERS:
    raise ValueError(
        "新拓扑要求 NUM_HRS == NUM_LAYERS（layer i 绑定 HRS[i]）,"
        f"当前 NUM_HRS={NUM_HRS}, NUM_LAYERS={NUM_LAYERS}"
    )

BASE_BW_GBPS = 400

# =========================
# Link delay macros
# =========================

# DELAY_HOST_TOR = "1ns"
# DELAY_TOR_TOR = "1ns"
# DELAY_TOR_DU = "5ns"
# DELAY_DU_L1 = "1ns"
# DELAY_DU_HRS = "250ns"

# FWD_DELAY_HOST = "1ns"
# FWD_DELAY_TOR = "100ns"
# FWD_DELAY_DU = "100ns"
# FWD_DELAY_L1 = "100ns"
# FWD_DELAY_HRS = "500ns"

DELAY_HOST_TOR = "1ns"
DELAY_TOR_TOR = "101ns"
DELAY_TOR_DU = "105ns"
DELAY_DU_L1 = "101ns"
DELAY_DU_HRS = "550ns"

FWD_DELAY_HOST = "0ns"
FWD_DELAY_TOR = "0ns"
FWD_DELAY_DU = "0ns"
FWD_DELAY_L1 = "0ns"
FWD_DELAY_HRS = "0ns"
# =========================
# Bandwidth macros
# =========================

BW_BASE = f"{BASE_BW_GBPS}Gbps"

# ToR 端口数量
# 1 host
# 7 ToR mesh（仅同列互连）
# 8 uplink（4 层 * 每层 2 个 DU）
#
# 解释：
# - 对于一个 8x8 Rack 内 ToR：
#   仅同列可连 (NUM_ROWS - 1) = 7 个 ToR
# - 再加 1 个下连 Host 端口 + 8 个上连 LRS 层端口
#   得到总端口数 16。
# - 这里将 Host<->ToR 带宽设为 (NUM_TOR_PORTS - 1) * BASE_BW_GBPS，
#   体现为接入侧聚合能力（不改变原有建模逻辑，仅解释设计意图）。

NUM_TOR_PORTS = 1 + (NUM_ROWS - 1) + (2 * NUM_LAYERS)

BW_HOST_TOR = f"{(NUM_TOR_PORTS - 1) * BASE_BW_GBPS}Gbps"
BW_TOR_TOR = BW_BASE
BW_TOR_DU = BW_BASE
BW_DU_L1 = BW_BASE
BW_DU_HRS = BW_BASE

# =========================
# Output config
# =========================

#OUTPUT_DIR = "./output/pod_lrs_hrs_v002"
OUTPUT_DIR = "/app/ns-3-ub/scratch/tp_UB_Mesh_V0002" # 4月1号下午，jyxiao改

PRIORITY_LIST = [7]

GEN_ROUTE_TABLE = True
GEN_TRANSPORT_CHANNEL = True

ROUTE_WORKERS = 8

MAX_SHORTEST_PATHS_PER_PAIR = 96


# =========================
# Shortest path routing
# =========================

def shortest_paths_for_routing(graph, source, target):
    """
    返回 host pair 的最短路径
    可以限制最大条数
    """

    try:
        paths = nx.all_shortest_paths(graph, source, target)

        if MAX_SHORTEST_PATHS_PER_PAIR is None:
            return list(paths)

        max_k = max(1, int(MAX_SHORTEST_PATHS_PER_PAIR))

        result = []

        for idx, p in enumerate(paths):
            # 验证点：当最短路径条数非常多时，这里会按 MAX_SHORTEST_PATHS_PER_PAIR 截断，
            # 以控制路由表规模和生成时间。
            if idx >= max_k:
                break

            result.append(p)

        return result

    except nx.NetworkXNoPath:
        return []


# =========================
# Build topology
# =========================

def build_topology():

    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = OUTPUT_DIR

    # ID 映射
    host_id_map = {}
    tor_id_map = {}
    du_id_map = {}
    l1_id_map = {}
    hrs_id_map = {}
    hrs_ids = []

    next_id = 0

    # 设计约定：
    # - 使用连续整数 ID 构建全网节点，便于后续导出配置和排障定位。
    # - 各 *_id_map 的 key 保留了拓扑坐标语义（pod/rack/row/col/layer/index），
    #   可在连线阶段直接按“空间位置”取节点 ID，降低拼接错误概率。

    # =====================================================
    # 1 添加 Host
    # =====================================================

    # 每个 (pod, rack, row, col) 放置 1 个 Host。
    # 验证点：应生成 NUM_POD * NUM_RACK * NUM_ROWS * NUM_COLS 个 Host。

    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for column in range(NUM_COLS):
                for row in range(NUM_ROWS):
                    key = (pod, rack, row, column)
                    host_id_map[key] = next_id
                    graph.add_netisim_host(next_id, forward_delay=FWD_DELAY_HOST) # host单独存储，方便后续生成路由表时区分 Host 与 Switch
                    next_id += 1

    # =====================================================
    # 2 添加 ToR
    # =====================================================

    # ToR 与 Host 在坐标上一一对应：同一个 (pod, rack, row, col) 位置有且仅有一个 ToR。
    # 这让 Host<->ToR 连接可以通过相同 key 直接映射。

    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for column in range(NUM_COLS):
                for row in range(NUM_ROWS):
                    key = (pod, rack, row, column)
                    tor_id_map[key] = next_id
                    graph.add_netisim_node(next_id, forward_delay=FWD_DELAY_TOR) # ToR 作为交换节点，使用 add_netisim_node 添加
                    next_id += 1

    # =====================================================
    # 3 添加 LRS (4 layers)
    # 每层 = 旧 LRS
    # =====================================================

    # LRS 分层设计说明：
    # - 每个 Rack 有 NUM_LAYERS 层，每层包含 NUM_DU_PER_LRS 个 DU 和 NUM_L1_PER_LRS 个 L1。
    # - 新拓扑中每层为 16 DU + 4 L1，DU 负责向上连接 HRS，L1 仅作为本层本 Rack 内部汇聚。
    # - layer 维度用于提供多条并行上行路径。
    # 验证点：
    # - DU 总数应为 NUM_POD * NUM_RACK * NUM_LAYERS * NUM_DU_PER_LRS
    # - L1 总数应为 NUM_POD * NUM_RACK * NUM_LAYERS * NUM_L1_PER_LRS

    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for layer in range(NUM_LAYERS):
                for du in range(NUM_DU_PER_LRS):
                    key = (pod, rack, layer, du)
                    du_id_map[key] = next_id
                    graph.add_netisim_node(next_id, forward_delay=FWD_DELAY_DU)
                    next_id += 1

 # 第二步：生成所有 L1（在所有 DU 之后）
    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for layer in range(NUM_LAYERS):
                for l1 in range(NUM_L1_PER_LRS):
                    key = (pod, rack, layer, l1)
                    l1_id_map[key] = next_id
                    graph.add_netisim_node(next_id, forward_delay=FWD_DELAY_L1)
                    next_id += 1

    # =====================================================
    # 4 添加 HRS
    # =====================================================

    # HRS 作为全局跨 Rack 汇聚节点。
    # 新拓扑中采用 layer 绑定：第 i 层 DU 仅连接 HRS[i]。
    # 验证点：hrs_ids 长度应等于 NUM_HRS。

    for layer in range(NUM_LAYERS):
        hrs_ids.append(next_id)
        graph.add_netisim_node(next_id, forward_delay=FWD_DELAY_HRS)
        key = (pod, layer)
        hrs_id_map[key] = next_id
        next_id += 1

    # =====================================================
    # 5 Host ↔ ToR
    # =====================================================

    # 一一映射连接：每个 Host 仅连接其同坐标 ToR。
    # 验证点：该阶段边数应等于 Host 数量（每个 Host 1 条接入链路）。

    for host_key, host_id in host_id_map.items():
        
        pod, rack, row, column = host_key
        tor_key = (pod, rack, row, column)
        tor_id = tor_id_map[tor_key]

        # add_netisim_edge(hid, tid, ...)
        # - hid: Host 全局 ID（端点1）
        # - tid: 与该 Host 同坐标的 ToR 全局 ID（端点2）
        graph.add_netisim_edge(
            host_id,
            tor_id,
            bandwidth=BW_HOST_TOR,
            delay=DELAY_HOST_TOR,
            edge_count=1,
        )

    # =====================================================
    # 6 ToR mesh (rack 内)
    # 仅列互连
    # =====================================================

    # ToR Mesh 原理（重点）：
    # - 在同一个 Rack 内，仅保留“同一列”的 ToR 互连。
    # - 因此每个 ToR 的 Rack 内邻居数 = (NUM_ROWS - 1)。
    # - 在 8x8 时即 7 个邻居。
    #
    # 去重策略（避免重复计数）:
    # - 对每对候选节点仅在 src < dst 时添加边，
    #   防止 (A,B) 与 (B,A) 被重复添加。
    #
    # 验证点（单 Rack）:
    # - 列内边数 = NUM_COLS * C(NUM_ROWS, 2)
    # - 总边数（全网） = NUM_POD * NUM_RACK * NUM_COLS * C(NUM_ROWS, 2)
    # 当前 8x8: 单 Rack 为 8*28 = 224 条。

    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for column in range(NUM_COLS):
                for src_row in range(NUM_ROWS):
                    src_tor = tor_id_map[(pod, rack, src_row, column)]
                    for dst_row in range(src_row+1, NUM_ROWS):
                        dst_tor = tor_id_map[(pod, rack, dst_row, column)]
                        # add_netisim_edge(src, dst, ...)
                        # - src/dst: 同 Rack、同一列内两个不同 ToR 的全局 ID
                        graph.add_netisim_edge(
                            src_tor,
                            dst_tor,
                            bandwidth=BW_TOR_TOR,
                            delay=DELAY_TOR_TOR,
                            edge_count=1,
                        )

    # =====================================================
    # 7 ToR ↔ DU
    # 每个 ToR 连接 4 个 layer
    # 第 i 列 ToR → 每层 DU[2i] 与 DU[2i+1]
    # =====================================================

    # 连接模式说明：
    # - 以“列”为绑定维度：col = i 的 ToR（该 Rack 内所有行）都上连到每层 2 个 DU。
    # - 每层连接目标为 DU[2i] 与 DU[2i+1]，即“列绑定 + 双 DU 冗余”。
    # - 因此每个 ToR（固定 row,col）会连到 4 层 * 2 = 8 个 DU 端点。
    #
    # 验证点：
    # - 每个 (pod, rack, col, layer) 组合会生成 NUM_ROWS * 2 条 ToR->DU 边。
    # - 总边数 = NUM_POD * NUM_RACK * NUM_COLS * NUM_LAYERS * NUM_ROWS * 2。

    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for column in range(NUM_COLS):
                for layer in range(NUM_LAYERS):
                    du_even = du_id_map[(pod, rack, layer, 2 * column)]
                    du_odd = du_id_map[(pod, rack, layer, 2 * column + 1)]
                    for row in range(NUM_ROWS):
                        tor = tor_id_map[(pod, rack, row, column)]

                        # add_netisim_edge(tor, du_even, ...)
                        # - tor    : (pod, rack, row, column) 的 ToR 全局 ID
                        # - du_even: (pod, rack, layer, 2*column) 的 DU 全局 ID
                        graph.add_netisim_edge(
                            tor,
                            du_even,
                            bandwidth=BW_TOR_DU,
                            delay=DELAY_TOR_DU,
                            edge_count=1,
                        )

                        # add_netisim_edge(tor, du_odd, ...)
                        # - tor: (pod, rack, row, column) 的 ToR 全局 ID
                        # - du_odd : (pod, rack, layer, 2*column+1) 的 DU 全局 ID
                        # 说明：同一列的所有 ToR（按 row 遍历）上连每层两个 DU。
                        graph.add_netisim_edge(
                            tor,
                            du_odd,
                            bandwidth=BW_TOR_DU,
                            delay=DELAY_TOR_DU,
                            edge_count=1,
                        )

    # =====================================================
    # 8 DU ↔ L1
    # 分组双链路连接
    # =====================================================

    # LRS 内部连接原理（重点）：
    # - 在同一 (pod, rack, layer) 内，DU 按奇偶分流到两组 L1：
    #   偶数 DU -> L1[0], L1[1]
    #   奇数 DU -> L1[2], L1[3]
    # - 每条 DU->L1 逻辑连接采用 edge_count=2（双物理链路）。
    #
    # 验证点（每层每 Rack，按 add_netisim_edge 调用次数）:
    # - 每个 DU 连接 2 个 L1 -> DU_PER_LRS * 2
    # - 每条连接 edge_count=2，若按物理链路数统计需再乘 2
    # - 总调用次数 = NUM_POD * NUM_RACK * NUM_LAYERS * DU_PER_LRS * 2
    # - 物理链路总数 = 上式 * 2

    for pod in range(NUM_POD):
        for rack in range(NUM_RACK):
            for layer in range(NUM_LAYERS):
                for du in range(NUM_DU_PER_LRS):
                    du_id = du_id_map[(pod, rack, layer, du)]

                    # 偶数 DU 连接前两台 L1，奇数 DU 连接后两台 L1。
                    if du_id % 2 == 0:
                        l1_targets = (0, 1)
                    else:
                        l1_targets = (2, 3)

                    for l1 in l1_targets:
                        # 含义：在同一 (pod, rack, layer) 内，定位编号为 l1 的 L1 节点全局 ID。
                        # 该 ID 与当前 du 组成一条 DU->L1 逻辑连接。
                        l1_id = l1_id_map[(pod, rack, layer, l1)]

                        # add_netisim_edge(du, l1, ...)
                        # - du: 当前 layer 内第 du_id 个 DU 全局 ID
                        # - l1: 当前 layer 内第 l1_id 个 L1 全局 ID
                        # - edge_count=2: 两条并行物理链路
                        graph.add_netisim_edge(
                            du_id,
                            l1_id,
                            bandwidth=BW_DU_L1,
                            delay=DELAY_DU_L1,
                            edge_count=2,
                        )

    # =====================================================
    # 9 DU ↔ HRS
    # layer 绑定直连
    # =====================================================

    # DU->HRS 原理（重点）：
    # - 每个 DU 只连接 1 个 HRS，且按 layer 绑定：DU(layer=i) -> HRS[i]
    # - 每条 DU->HRS 逻辑连接采用 edge_count=4（四条并行物理链路）
    # - 这是全网唯一跨 Rack 的上层通道。
    #
    # 验证点：
    # - add_netisim_edge 调用次数 = NUM_POD * RACK_NUM * NUM_LAYERS * NUM_DU_PER_LRS
    # - 物理链路总数 = 上式 * 4

    for pod in range(NUM_POD):
        for layer in range(NUM_LAYERS):
            key = (pod, layer)
            hrs_id = hrs_id_map[key]
            for rack in range(NUM_RACK):
                for du in range(NUM_DU_PER_LRS):

                    # 含义：根据 (pod, rack, layer, du_idx) 四维坐标，取该 DU 节点全局 ID。
                    du_id = du_id_map[(pod, rack, layer, du)]

                    # add_netisim_edge(du, hrs, ...)
                    # - du : 当前 Rack/Layer/Index 对应的 DU 全局 ID
                    # - hrs: 与该 layer 绑定的 HRS 全局 ID（hrs_id）
                    # - edge_count=4: 四条并行物理链路
                    graph.add_netisim_edge(
                        du_id,
                        hrs_id,
                        bandwidth=BW_DU_HRS,
                        delay=DELAY_DU_HRS,
                        edge_count=4,
                    )

    return graph


# =====================================================
# Generate config
# =====================================================

def generate_csv(graph):

    os.makedirs(graph.output_dir, exist_ok=True)

    graph.build_graph_config()

    # 先固化图结构，再生成路由与通道配置。
    # 验证点：若上游拓扑构建有误，这里通常会在后续路由生成阶段暴露（如无路径/规模异常）。

    if GEN_ROUTE_TABLE:

        graph.gen_route_table(
            write_file=True,
            host_router=True,
            path_finding_algo=shortest_paths_for_routing,
            multiple_workers=ROUTE_WORKERS,
        )

    if GEN_TRANSPORT_CHANNEL:

        graph.config_transport_channel(priority_list=PRIORITY_LIST)

    graph.write_config()


# =====================================================
# main
# =====================================================

def main():

    graph = build_topology()

    print(
        f"Nodes total: {graph.get_total_num()} "
        f"(hosts={graph.get_host_num()}, switches={graph.get_node_num()})"
    )

    # 快速人工验证建议：
    # - 当前参数下 Nodes total 应为 836（256 host + 580 switch/node）。
    # - 若不一致，优先检查宏参数和各阶段循环上界。

    print(f"Output dir: {os.path.abspath(graph.output_dir)}")

    print(
        "Tip: all shortest paths are used by default. "
        "Set MAX_SHORTEST_PATHS_PER_PAIR to limit runtime."
    )

    generate_csv(graph)


if __name__ == "__main__":
    main()