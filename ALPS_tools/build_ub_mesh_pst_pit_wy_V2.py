#!/usr/bin/env python3
"""根据 UB Mesh 拓扑生成 ALPS 所需的 PIT/PST 表。

【功能概述】
该脚本面向 UB Mesh 组网场景，将拓扑描述文件转换为 ALPS 路由子系统可直接消费的两张路径表：
1) PIT (Path Index Table): 记录每条路径的节点序列、出端口序列、反向路径 ID 与基础时延。
2) PST (Path Search Table): 记录每个源/宿主机对可用的路径 ID 列表。

【输入文件】
1) node.csv
    - nodeId: 节点 ID 或区间表达式 (如 0..255)
    - nodeType: 节点类型 (DEVICE / SWITCH 等)
    - forwardDelay: 节点内部转发时延 (支持 ns/us/ms/s)
2) topology.csv
    - nodeId1, portId1, nodeId2, portId2: 有向端口连接关系
    - delay: 链路传播时延 (支持 ns/us/ms/s)
    - 支持同一对节点之间多条并行链路 (通过端口/索引区分)

【输出文件】
1) path_index_table.csv
    字段: path_id, path_length, traverse_nodes, output_ports, reverse_path_id, total_base_delay_ns
2) path_search_table.csv
    字段: sourceNode, destNode, num_paths, path_ids

【路由建模规则】
1) 同 rack 同列(不同行): 按模板最多 7 条。
2) 同 rack 不同列: 按固定规则最多 8 条。
3) 同 pod 不同 rack: 按固定规则最多 32 条。
4) 路径唯一性采用“端口敏感”判定: 节点序列相同但端口不同视为不同路径。
5) 若模板路径在物理拓扑中缺边或选定并行链路索引不存在，则该路径作废并跳过。

【时延模型】
路径基础时延 = 节点转发时延总和 + 链路传播时延总和。
当前实现不叠加排队、拥塞、重传等动态时延，仅用于静态路径基准对比。
"""

import argparse
import csv
from dataclasses import dataclass
from itertools import combinations
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


# ---------------------------
# 拓扑结构参数（物理含义）
# ---------------------------
# Pod 数量。一个 Pod 可理解为独立的大区/机房单元。
POD_NUM = 1
# 每个 Pod 内 rack 数量。rack 是机架级故障域。
RACK_NUM = 4
# 每个 rack 的主机行数（二维网格中的纵向坐标）。
RACK_ROWS = 8
# 每个 rack 的主机列数（二维网格中的横向坐标）。
RACK_COLS = 8
# 每个 rack 内 LRS 分层数，可理解为上行汇聚分层维度。
LRS_LAYERS = 4
# 每层 LRS 内 DU 数量（Device Unit，接入/中继单元）。
DU_PER_LRS = 16
# 每层 LRS 内 L1 数量（一级汇聚节点）。
L1_PER_LRS = 4
# HRS 节点总数（High-level Relay Switch，跨 rack 汇聚核心）。
HRS_NUM = 4
# 同 rack 不同列路径的权重缩放（相较于同列路径），用于 ALPS 路径选择概率计算。
SAME_RACK_DIFF_COL_WEIGHT=0.75
# 同 pod 跨 rack 路径的权重缩放
SAME_POD_DIFF_RACK_WEIGHT=0.5   

# key: (rack_0based, col_0based), value: Index 列表
PORT_INDEX_MAP = {
    (0, 0): [0, 2, 3, 7, 1, 4, 5, 6],  # Rack 1 Column 1
    (0, 1): [0, 1, 4, 7, 2, 3, 5, 6],  # Rack 1 Column 2
    (0, 2): [0, 3, 6, 7, 1, 2, 4, 5],  # Rack 1 Column 3
    (0, 3): [0, 4, 6, 7, 1, 2, 3, 5],  # Rack 1 Column 4
    (0, 4): [0, 2, 3, 5, 1, 4, 6, 7],  # Rack 1 Column 5
    (0, 5): [0, 1, 4, 7, 2, 3, 5, 6],  # Rack 1 Column 6
    (0, 6): [0, 5, 6, 7, 1, 2, 3, 4],  # Rack 1 Column 7
    (0, 7): [0, 3, 4, 6, 1, 2, 5, 7],  # Rack 1 Column 8
    
    (1, 0): [0, 1, 4, 6, 2, 3, 5, 7],  # Rack 2 Column 1
    (1, 1): [0, 2, 5, 7, 1, 3, 4, 6],  # Rack 2 Column 2
    (1, 2): [0, 4, 6, 7, 1, 2, 3, 5],  # Rack 2 Column 3
    (1, 3): [0, 4, 5, 7, 1, 2, 3, 6],  # Rack 2 Column 4
    (1, 4): [0, 1, 5, 7, 2, 3, 4, 6],  # Rack 2 Column 5
    (1, 5): [0, 2, 5, 6, 1, 3, 4, 7],  # Rack 2 Column 6
    (1, 6): [0, 2, 4, 5, 1, 3, 6, 7],  # Rack 2 Column 7
    (1, 7): [0, 1, 4, 6, 2, 3, 5, 7],  # Rack 2 Column 8
    
    (2, 0): [0, 1, 3, 4, 2, 5, 6, 7],  # Rack 3 Column 1
    (2, 1): [0, 1, 2, 3, 4, 5, 6, 7],  # Rack 3 Column 2
    (2, 2): [0, 2, 3, 4, 1, 5, 6, 7],  # Rack 3 Column 3
    (2, 3): [0, 1, 2, 7, 3, 4, 5, 6],  # Rack 3 Column 4
    (2, 4): [0, 1, 4, 5, 2, 3, 6, 7],  # Rack 3 Column 5
    (2, 5): [0, 4, 5, 6, 1, 2, 3, 7],  # Rack 3 Column 6
    (2, 6): [0, 4, 5, 7, 1, 2, 3, 6],  # Rack 3 Column 7
    (2, 7): [0, 2, 3, 5, 1, 4, 6, 7],  # Rack 3 Column 8
    
    (3, 0): [0, 2, 5, 6, 1, 3, 4, 7],  # Rack 4 Column 1
    (3, 1): [0, 1, 5, 6, 2, 3, 4, 7],  # Rack 4 Column 2
    (3, 2): [0, 1, 6, 7, 2, 3, 4, 5],  # Rack 4 Column 3
    (3, 3): [0, 2, 3, 7, 1, 4, 5, 6],  # Rack 4 Column 4
    (3, 4): [0, 1, 5, 7, 2, 3, 4, 6],  # Rack 4 Column 5
    (3, 5): [0, 5, 6, 7, 1, 2, 3, 4],  # Rack 4 Column 6
    (3, 6): [0, 4, 5, 7, 1, 2, 3, 6],  # Rack 4 Column 7
    (3, 7): [0, 3, 6, 7, 1, 2, 4, 5],  # Rack 4 Column 8
}
if HRS_NUM != LRS_LAYERS:
    raise ValueError(
        "Expected HRS_NUM == LRS_LAYERS in new topology, "
        f"got HRS_NUM={HRS_NUM}, LRS_LAYERS={LRS_LAYERS}"
    )


@dataclass(frozen=True)
class HostLoc:
    """主机逻辑坐标：pod、rack、行、列。"""

    pod: int
    rack: int
    rack_r: int
    rack_c: int


@dataclass
class PathEntry:
    """可写入 PIT 的路径对象。"""

    nodes: List[int]
    ports: List[int]
    edge_indices: List[int]
    total_base_delay_ns: float
    weight: float = 1.0

    @property
    def path_length(self) -> int:
        return len(self.nodes) - 1


# 邻接边映射。
# key: (u, v)
# value: [(out_port, delay_ns), ...]，同一有向边可有多条并行物理链路。
EdgeMap = Dict[Tuple[int, int], List[Tuple[int, float]]]


def parse_node_id_expr(expr: str) -> Iterable[int]:
    """解析 nodeId 字段（单值或区间）。"""
    expr = expr.strip()
    if ".." in expr:
        start, end = map(int, expr.split(".."))
        return range(start, end + 1)
    return [int(expr)]


def parse_delay_ns(text: str) -> float:
    """将时延文本统一转换为 ns(float)。"""
    s = text.strip().lower()
    if s.endswith("ns"):
        return float(s[:-2])
    if s.endswith("us"):
        return float(s[:-2]) * 1_000.0
    if s.endswith("ms"):
        return float(s[:-2]) * 1_000_000.0
    if s.endswith("s"):
        return float(s[:-1]) * 1_000_000_000.0
    return float(s)


def load_nodes(node_csv: Path) -> Tuple[List[int], Dict[int, float]]:
    """读取 node.csv，返回主机列表与节点转发时延表。"""
    hosts: List[int] = []
    node_forward_delay: Dict[int, float] = {}
    with node_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            node_type = row["nodeType"].strip().upper()  # 统一大小写，规避输入不规范。
            fwd_delay_ns = parse_delay_ns(row["forwardDelay"])  # 节点内部处理时延（ns）。
            for nid in parse_node_id_expr(row["nodeId"]):
                node_forward_delay[nid] = fwd_delay_ns  # 所有节点都记录转发时延，供路径时延累加。
                if node_type == "DEVICE":
                    hosts.append(nid)  # DEVICE 视为端主机，参与源宿组合枚举。
    return sorted(hosts), node_forward_delay


def load_topology(topo_csv: Path) -> EdgeMap:
    """读取 topology.csv，构建端口敏感的双向多重图。"""
    edge_map: EdgeMap = {}
    with topo_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            n1 = int(row["nodeId1"])
            p1 = int(row["portId1"])
            n2 = int(row["nodeId2"])
            p2 = int(row["portId2"])
            dly = parse_delay_ns(row["delay"])  # 链路传播时延（ns）。
            edge_map.setdefault((n1, n2), []).append((p1, dly))  # 正向边：记录 n1->n2 的出端口。
            edge_map.setdefault((n2, n1), []).append((p2, dly))  # 反向边：记录 n2->n1 的出端口。
    return edge_map


def host_offset(loc: HostLoc) -> int:
    """将主机坐标映射为线性偏移。"""
    #目前是列增加形式-------3月25号下午，jyxiao改
    hosts_per_rack = RACK_ROWS * RACK_COLS
    in_rack_idx = (loc.rack_c * RACK_ROWS) + loc.rack_r
    return loc.pod * RACK_NUM * hosts_per_rack + loc.rack * hosts_per_rack + in_rack_idx

def host_id(loc: HostLoc) -> int:
    """计算主机全局 ID。"""
    return host_offset(loc)


def tor_id(loc: HostLoc) -> int:
    """计算 ToR 全局 ID。"""
    total_hosts = POD_NUM * RACK_NUM * RACK_ROWS * RACK_COLS
    return total_hosts + host_offset(loc)


def _du_base_id() -> int:
    """返回 DU ID 段起始偏移。"""
    total_hosts = POD_NUM * RACK_NUM * RACK_ROWS * RACK_COLS
    total_tors = total_hosts
    return total_hosts + total_tors


def _l1_base_id() -> int:
    """返回 L1 ID 段起始偏移。"""
    total_du = POD_NUM * RACK_NUM * LRS_LAYERS * DU_PER_LRS
    return _du_base_id() + total_du


def _hrs_base_id() -> int:
    """返回 HRS ID 段起始偏移。"""
    total_l1 = POD_NUM * RACK_NUM * LRS_LAYERS * L1_PER_LRS
    return _l1_base_id() + total_l1


def du_id(pod: int, rack: int, layer: int, du_idx: int) -> int:
    """计算 DU 全局 ID。"""
    if du_idx < 0 or du_idx >= DU_PER_LRS:
        raise ValueError(f"du_idx out of range: {du_idx}")
    du_flat = (((pod * RACK_NUM + rack) * LRS_LAYERS + layer) * DU_PER_LRS) + du_idx
    return _du_base_id() + du_flat


def l1_id(pod: int, rack: int, layer: int, l1_idx: int) -> int:
    """计算 L1 全局 ID。"""
    if l1_idx < 0 or l1_idx >= L1_PER_LRS:
        raise ValueError(f"l1_idx out of range: {l1_idx}")
    l1_flat = (((pod * RACK_NUM + rack) * LRS_LAYERS + layer) * L1_PER_LRS) + l1_idx
    return _l1_base_id() + l1_flat


def hrs_id(hrs_idx: int) -> int:
    """计算 HRS 全局 ID。"""
    if hrs_idx < 0 or hrs_idx >= HRS_NUM:
        raise ValueError(f"hrs_idx out of range: {hrs_idx}")
    return _hrs_base_id() + hrs_idx


# def decode_host_loc(hid: int) -> HostLoc:
#     """将主机 ID 反解为逻辑坐标。"""
#     hosts_per_rack = RACK_ROWS * RACK_COLS
#     hosts_per_pod = hosts_per_rack * RACK_NUM

#     pod = hid // hosts_per_pod
#     in_pod = hid % hosts_per_pod

#     rack_idx = in_pod // hosts_per_rack
#     in_rack = in_pod % hosts_per_rack

#     rack_r = in_rack // RACK_COLS
#     rack_c = in_rack % RACK_COLS

#     return HostLoc(pod, rack_idx, rack_r, rack_c)
def decode_host_loc(hid: int) -> HostLoc:
    """将主机 ID 反解为逻辑坐标。"""
     #目前是列增加形式-------3月25号下午，jyxiao改
    hosts_per_rack = RACK_ROWS * RACK_COLS
    hosts_per_pod = hosts_per_rack * RACK_NUM

    pod = hid // hosts_per_pod
    in_pod = hid % hosts_per_pod

    rack_idx = in_pod // hosts_per_rack
    in_rack = in_pod % hosts_per_rack

    rack_c = in_rack // RACK_ROWS
    rack_r = in_rack % RACK_ROWS

    return HostLoc(pod, rack_idx, rack_r, rack_c)


def pick_edge(
    u: int,
    v: int,
    edge_map: EdgeMap,
    selected_idx: Optional[int],
) -> Optional[Tuple[int, int, float]]:
    """从 (u,v) 的并行边集合中选择一条。"""
    edges = edge_map.get((u, v), [])
    if not edges:
        return None

    idx = 0 if selected_idx is None else selected_idx  # 未指定时默认选第 0 条并行边。
    if idx < 0 or idx >= len(edges):
        return None

    out_port, dly = edges[idx]  # 返回端口与时延，供 PIT 编码与时延累加。
    return idx, out_port, dly


def path_from_nodes(
    nodes: Sequence[int],
    edge_map: EdgeMap,
    node_fwd_delay: Dict[int, float],
    hop_choice: Optional[Dict[int, int]] = None,
) -> Optional[PathEntry]:
    """将节点序列实例化为具体路径（含端口/边索引/时延）。"""
    ports: List[int] = []  # 每一跳对应的出端口序列。
    edge_indices: List[int] = []  # 每一跳在并行边列表中的选中索引。
    link_delay_sum = 0.0  # 链路传播时延累加。

    for hop_idx, (u, v) in enumerate(zip(nodes[:-1], nodes[1:])):
        selected_idx = None if hop_choice is None else hop_choice.get(hop_idx)  # 可按跳指定并行边。
        picked = pick_edge(u, v, edge_map, selected_idx)  # 从物理拓扑中解析该跳。
        if picked is None:
            print(f"\n{u}->{v} is unreachable")
            return None  # 任意一跳不可达则整条模板路径失效。
        edge_idx, out_port, dly = picked
        edge_indices.append(edge_idx)  # 记录并行边索引，用于反向映射与调试。
        ports.append(out_port)  # 记录该跳出端口，PIT 以端口序列标识路径行为。
        link_delay_sum += dly  # 累加链路传播时延。

    forward_sum = sum(node_fwd_delay.get(n, 0.0) for n in nodes)  # 累加节点内部转发时延。
    if any(n not in node_fwd_delay for n in nodes):
        print(f"\nnode_fwd_delay is incomplete: {node_fwd_delay}")
        return None  # 转发时延缺失视为输入不完整，不生成该路径。

    return PathEntry(list(nodes), ports, edge_indices, forward_sum + link_delay_sum)


def dedupe_paths(paths: Sequence[PathEntry]) -> List[PathEntry]:
    """按“节点序列+端口序列”去重并稳定排序。"""
    uniq: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], PathEntry] = {}
    for p in paths:
        key = (tuple(p.nodes), tuple(p.ports))  # 端口敏感去重键。
        if key not in uniq:
            uniq[key] = p

    return sorted(
        uniq.values(),
        key=lambda p: (p.path_length, p.total_base_delay_ns, tuple(p.nodes), tuple(p.ports)),
    )


def pick_src_du_l1_rule_same_rack(src_col: int, src_row: int) -> Tuple[int, int, int]:
    """同 rack 不同列：根据源坐标选择源 DU、L1 与 DU->L1 链路索引。"""
    if src_row < 4:
        du_idx = 2 * src_col
        l1_idx = 0 if src_row < 2 else 1
        link_idx = src_row % 2
    else:
        du_idx = 2 * src_col + 1
        l1_idx = 2 if src_row < 6 else 3
        link_idx = (src_row - 4) % 2
    
    return du_idx, l1_idx, link_idx


def pick_dst_du_same_rack(src_l1_idx: int, dst_col: int) -> int:
    """同 rack 不同列：根据源 L1 分组选择目标 DU。"""
    if src_l1_idx in (0, 1):
        return 2 * dst_col
    else:
        return 2 * dst_col + 1


def pick_src_du_rule_cross_rack(src_col: int, src_row: int) -> Tuple[int, int]:
    """同 pod 跨 rack：根据源坐标选择源 DU 与 DU->HRS 链路索引。"""
    if src_row < 4:
        du_idx = 2 * src_col + 1
        link_idx = src_row
    else:
        du_idx = 2 * src_col
        link_idx = src_row - 4
    
    return du_idx, link_idx


def build_same_rack_same_col_paths(
    src: HostLoc,
    dst: HostLoc,
    edge_map: EdgeMap,
    node_fwd_delay: Dict[int, float],
) -> Tuple[List[PathEntry], int, int]:
    """构造“同 rack 同列(不同行)”路径集合，上限 7 条。"""
    src_h = host_id(src)
    dst_h = host_id(dst)
    src_t = tor_id(src)
    dst_t = tor_id(dst)

    candidates: List[PathEntry] = []

    # 路径模板 1：直达（源主机 -> 源 ToR -> 目的 ToR -> 目的主机）。
    direct = path_from_nodes([src_h, src_t, dst_t, dst_h], edge_map, node_fwd_delay)
    if direct is not None:
        candidates.append(direct)

    # 路径模板 2~7：经“同列中间行 ToR”绕行，每个可用中间行贡献 1 条候选路径。
    for mid_r in range(RACK_ROWS):
        if mid_r in (src.rack_r, dst.rack_r):
            continue
        mid_t = tor_id(HostLoc(src.pod, src.rack, mid_r, src.rack_c))
        p = path_from_nodes([src_h, src_t, mid_t, dst_t, dst_h], edge_map, node_fwd_delay)
        if p is not None:
            candidates.append(p)

    valid = dedupe_paths(candidates)  # 端口敏感去重，保证输出稳定。
    return valid, 7, len(valid)



def build_same_rack_diff_col_paths(
    src: HostLoc,
    dst: HostLoc,
    edge_map: EdgeMap,
    node_fwd_delay: Dict[int, float],
) -> Tuple[List[PathEntry], int, int]:
    """构造"同 rack 不同列"路径集合，上限 8 条。"""
    src_h = host_id(src)
    dst_h = host_id(dst)
    src_t = tor_id(src)
    dst_t = tor_id(dst)

    # 根据源主机位置确定上行接入点（源 DU）及 L1 扇区。
    src_du_idx, src_l1_idx, src_du_l1_link_idx = pick_src_du_l1_rule_same_rack(src.rack_c, src.rack_r)
    # 根据源 L1 扇区与目的列确定目的 DU。
    dst_du_idx = pick_dst_du_same_rack(src_l1_idx, dst.rack_c)

    # 获取目的 Column 的端口索引映射序列（用于 Layer 0/1 偶数 DU 的路由决策）。
    port_index = PORT_INDEX_MAP.get((dst.rack, dst.rack_c))
    if port_index is None:
        # 如果找不到配置，回退到简单直连模式。
        port_index = list(range(8))
    else:
        portMap = {0:{}, 1:{}}
        portMap[0]["cross_rack"] =  port_index[4:8]
        portMap[0]["cross_colunm"] = port_index[0:4] 
        portMap[1]["cross_rack"] = port_index[0:4]
        portMap[1]["cross_colunm"] = port_index[4:8]

    candidates: List[PathEntry] = []

    for layer in range(LRS_LAYERS):
        src_du = du_id(src.pod, src.rack, layer, src_du_idx)
        src_l1 = l1_id(src.pod, src.rack, layer, src_l1_idx)
        dst_du = du_id(dst.pod, dst.rack, layer, dst_du_idx)

        # Layer 0/1且使用偶数DU时，应用新的DU->ToR路由规则。
        use_new_rule = (layer in (0, 1) and dst_du_idx % 2 == 0)
        port_index_base = 0 if src_l1%2 ==0 else 2
        for l1_dst_du_link_idx in (0, 1):
            if use_new_rule:
                # 根据 layer、目的行、l1_dst_du_link_idx 确定 DU->ToR 的中间 ToR 索引。
                dst_row = dst.rack_r
                mid_tor_row = portMap[layer]["cross_colunm"][port_index_base + l1_dst_du_link_idx]
                    
                # 构建节点序列。
                if mid_tor_row == dst_row:
                    nodes = [src_h, src_t, src_du, src_l1, dst_du, dst_t, dst_h]
                else:
                    mid_t = tor_id(HostLoc(dst.pod, dst.rack, mid_tor_row, dst.rack_c))
                    nodes = [src_h, src_t, src_du, src_l1, dst_du, mid_t, dst_t, dst_h]
            else:
                # Layer 2/3或奇数DU：保持原规则，DU->ToR直连。
                nodes = [src_h, src_t, src_du, src_l1, dst_du, dst_t, dst_h]
                
            hop_choice = {
                2: src_du_l1_link_idx,
                3: l1_dst_du_link_idx,
            }
            p = path_from_nodes(nodes, edge_map, node_fwd_delay, hop_choice)
            if p is not None:
                if not use_new_rule:
                    # 下行到DU有2条候选但DU->ToR单出口时，降低初始权重。
                    p.weight = SAME_RACK_DIFF_COL_WEIGHT
                candidates.append(p)

    valid = dedupe_paths(candidates)
    return valid, 8, len(valid)


# ... existing code ...


def build_same_pod_diff_rack_paths(
    src: HostLoc,
    dst: HostLoc,
    edge_map: EdgeMap,
    node_fwd_delay: Dict[int, float],
) -> Tuple[List[PathEntry], int, int]:
    """构造"同 pod 不同 rack"路径集合，上限 32 条。"""
    src_h = host_id(src)
    dst_h = host_id(dst)
    src_t = tor_id(src)
    dst_t = tor_id(dst)

    # 源侧按源坐标固定选择 DU 与 DU->HRS 上行链路。
    src_du_idx, src_du_hrs_link_idx = pick_src_du_rule_cross_rack(src.rack_c, src.rack_r)

    # 获取目的 Column 的端口索引映射序列（用于 Layer 0/1 偶数 DU 的路由决策）。
    port_index = PORT_INDEX_MAP.get((dst.rack, dst.rack_c))
    if port_index is None:
        # 如果找不到配置，回退到简单直连模式。
        port_index = list(range(8))
    else:
        portMap = {0:{}, 1:{}}
        portMap[0]["cross_rack"] =  port_index[4:8]
        portMap[0]["cross_colunm"] = port_index[0:4] 
        portMap[1]["cross_rack"] = port_index[0:4]
        portMap[1]["cross_colunm"] = port_index[4:8]

    candidates: List[PathEntry] = []

    for layer in range(LRS_LAYERS):
        src_du = du_id(src.pod, src.rack, layer, src_du_idx)
        hrs = hrs_id(layer)

        # Layer 0/1且使用偶数DU时，应用新的DU->ToR路由规则。
        is_target_layer = (layer in (0, 1) and True)  # 当前 layer 的目标 DU 由 src_l1_idx 决定，可能是偶数或奇数
        
        # 遍历目的列对应的两类 DU（偶数/奇数）。
        for dst_du_idx in (2 * dst.rack_c, 2 * dst.rack_c + 1):
            dst_du = du_id(dst.pod, dst.rack, layer, dst_du_idx)
            
            # 判断当前 DU 是否为偶数 DU 且需要应用新规则。
            is_even_du = (dst_du_idx % 2 == 0) # 从0开始，偶数 DU 的索引为偶数，奇数 DU 的索引为奇数
            apply_new_rule = is_target_layer and is_even_du

            # HRS->目的 DU 提供 4 条并行链路索引。
            for hrs_dst_du_link_idx in range(4):
                if apply_new_rule:
                    mid_tor_row = portMap[layer]["cross_rack"][hrs_dst_du_link_idx]
                    dst_row = dst.rack_r # 目标NPU所在的行序号
                    # 构建节点序列。
                    if mid_tor_row == dst_row:
                        nodes = [src_h, src_t, src_du, hrs, dst_du, dst_t, dst_h]
                    else:
                        mid_t = tor_id(HostLoc(dst.pod, dst.rack, mid_tor_row, dst.rack_c))
                        nodes = [src_h, src_t, src_du, hrs, dst_du, mid_t, dst_t, dst_h]
                    
                    hop_choice = {
                        2: src_du_hrs_link_idx,
                        3: hrs_dst_du_link_idx,
                    }
                    p = path_from_nodes(nodes, edge_map, node_fwd_delay, hop_choice)
                    if p is not None:
                        candidates.append(p)
                
                else:
                    # Layer 2/3 或奇数 DU：保持原规则，DU->ToR 直连。
                    nodes = [src_h, src_t, src_du, hrs, dst_du, dst_t, dst_h]
                    
                    hop_choice = {
                        2: src_du_hrs_link_idx,
                        3: hrs_dst_du_link_idx,
                    }
                    p = path_from_nodes(nodes, edge_map, node_fwd_delay, hop_choice)
                    if p is not None:
                        # 下行到DU有4条候选但DU->ToR单出口时，降低初始权重。
                        p.weight = SAME_POD_DIFF_RACK_WEIGHT
                        candidates.append(p)

    valid = dedupe_paths(candidates)
    return valid, 32, len(valid)


# ... existing code ...

def build_cross_pod_paths_placeholder(
    _src: HostLoc,
    _dst: HostLoc,
    _edge_map: EdgeMap,
    _node_fwd_delay: Dict[int, float],
) -> Tuple[List[PathEntry], int, int]:
    """跨 pod 场景占位实现（当前未启用）。"""
    return [], 0, 0


def build_pair_paths(
    src_hid: int,
    dst_hid: int,
    edge_map: EdgeMap,
    node_fwd_delay: Dict[int, float],
) -> Tuple[List[PathEntry], int, int, str]:
    """按源宿主机位置选择场景并构造路径。"""
    src = decode_host_loc(src_hid)
    dst = decode_host_loc(dst_hid)

    # 一级分支：先按 pod/rack 归类业务场景，再调用对应模板构造器。
    if (src.pod, src.rack) == (dst.pod, dst.rack):
        if src.rack_c == dst.rack_c and src.rack_r != dst.rack_r:
            paths, expected, actual = build_same_rack_same_col_paths(src, dst, edge_map, node_fwd_delay)
            return paths, expected, actual, "same_rack_same_col"

        paths, expected, actual = build_same_rack_diff_col_paths(src, dst, edge_map, node_fwd_delay)
        return paths, expected, actual, "same_rack_diff_col"

    if src.pod == dst.pod:
        paths, expected, actual = build_same_pod_diff_rack_paths(src, dst, edge_map, node_fwd_delay)
        return paths, expected, actual, "same_pod_diff_rack"

    paths, expected, actual = build_cross_pod_paths_placeholder(src, dst, edge_map, node_fwd_delay)
    return paths, expected, actual, "cross_pod_placeholder"


def build_reverse_path(forward: PathEntry, edge_map: EdgeMap, node_fwd_delay: Dict[int, float]) -> Optional[PathEntry]:
    """根据前向路径生成一条端口一致的反向路径。"""
    reversed_nodes = list(reversed(forward.nodes))
    reversed_indices = list(reversed(forward.edge_indices))
    # 反向路径需沿用“反向后每一跳对应的并行边索引”，确保正反路径可互指。
    hop_choice = {i: idx for i, idx in enumerate(reversed_indices)}
    reversed_path = path_from_nodes(reversed_nodes, edge_map, node_fwd_delay, hop_choice)
    if reversed_path is not None:
        reversed_path.weight = forward.weight
    return reversed_path


def format_int_list(values: Sequence[int]) -> str:
    """将整数序列编码为空格分隔字符串。"""
    return " ".join(str(v) for v in values)


# ... existing code ...

def write_tables(case_dir: Path) -> None:
    """执行主流程：读入拓扑、生成 PIT/PST、输出统计与校验信息。"""
    node_csv = case_dir / "node.csv"  # 节点属性输入。
    topo_csv = case_dir / "topology.csv"  # 链路拓扑输入。
    pit_csv = case_dir / "path_index_table.csv"  # 路径明细输出。
    pst_csv = case_dir / "path_search_table.csv"  # 路径索引输出。

    if not node_csv.exists() or not topo_csv.exists():
        raise FileNotFoundError("case_dir must contain node.csv and topology.csv")

    hosts, node_fwd_delay = load_nodes(node_csv)
    edge_map = load_topology(topo_csv)

    mismatch_logs: List[str] = []  # 记录模板上限差异与正反向映射异常。
    total_pairs = 0  # 已处理无序主机对数量。
    total_paths = 0  # 已写入 PIT 的总路径条数（含所有方向）。
    path_id_counter = 0  # 全局递增 path_id 游标。

    scenario_counter: Dict[str, int] = {
        "same_rack_same_col": 0,
        "same_rack_diff_col": 0,
        "same_pod_diff_rack": 0,
        "cross_pod_placeholder": 0,
    }

    with pit_csv.open("w", encoding="utf-8", newline="") as f_pit, pst_csv.open(
        "w", encoding="utf-8", newline=""
    ) as f_pst:
        pit_writer = csv.writer(f_pit)
        pst_writer = csv.writer(f_pst)

        pit_writer.writerow(
            [
                "path_id",
                "path_length",
                "traverse_nodes",
                "output_ports",
                "reverse_path_id",
                "total_base_delay_ns",
                "weight",
            ]
        )
        pst_writer.writerow(["sourceNode", "destNode", "num_paths", "path_ids"])

        for src, dst in combinations(hosts, 2):
            total_pairs += 1  # combinations 仅产出 src<dst 的无序对。

            # ========== 方向 1: src -> dst (使用 dst 的 Column Index) ==========
            forward_paths_AB, expected_upper_AB, actual_count_AB, scenario_AB = build_pair_paths(
                src, dst, edge_map, node_fwd_delay
            )
            scenario_counter[scenario_AB] += 1

            if expected_upper_AB != 0 and actual_count_AB != expected_upper_AB:
                mismatch_logs.append(
                    f"pair({src},{dst}) AB scenario={scenario_AB} upper={expected_upper_AB}, actual={actual_count_AB}"
                )

            fwd_AB = dedupe_paths(forward_paths_AB)  # A→B 主路径去重与稳定排序。

            # 为 A→B 主路径生成反向路径集合（用于 ACK 从 B→A）。
            rev_AB_generated: List[PathEntry] = []
            fwd_AB_to_rev_key: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], Tuple[Tuple[int, ...], Tuple[int, ...]]] = {}
            for p in fwd_AB:
                rp = build_reverse_path(p, edge_map, node_fwd_delay)
                if rp is None:
                    continue
                rev_AB_generated.append(rp)
                f_key = (tuple(p.nodes), tuple(p.ports))
                r_key = (tuple(rp.nodes), tuple(rp.ports))
                fwd_AB_to_rev_key[f_key] = r_key

            rev_AB = dedupe_paths(rev_AB_generated)  # A→B 的反向路径去重。

            # ========== 方向 2: dst -> src (使用 src 的 Column Index) ==========
            forward_paths_BA, _, _, _ = build_pair_paths(
                dst, src, edge_map, node_fwd_delay
            )

            fwd_BA = dedupe_paths(forward_paths_BA)  # B→A 主路径去重与稳定排序。

            # 为 B→A 主路径生成反向路径集合（用于 ACK 从 A→B）。
            rev_BA_generated: List[PathEntry] = []
            fwd_BA_to_rev_key: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], Tuple[Tuple[int, ...], Tuple[int, ...]]] = {}
            for p in fwd_BA:
                rp = build_reverse_path(p, edge_map, node_fwd_delay)
                if rp is None:
                    continue
                rev_BA_generated.append(rp)
                f_key = (tuple(p.nodes), tuple(p.ports))
                r_key = (tuple(rp.nodes), tuple(rp.ports))
                fwd_BA_to_rev_key[f_key] = r_key

            rev_BA = dedupe_paths(rev_BA_generated)  # B→A 的反向路径去重。

            # ========== 写入 PIT 和 PST ==========
            # 为四个路径集合分配 path_id 区间。
            # 区间布局：[fwd_AB] [rev_AB] [fwd_BA] [rev_BA]
            base_fwd_AB = path_id_counter
            base_rev_AB = base_fwd_AB + len(fwd_AB)
            base_fwd_BA = base_rev_AB + len(rev_AB)
            base_rev_BA = base_fwd_BA + len(fwd_BA)

            # 构建反向路径索引映射。
            rev_AB_idx: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], int] = {
                (tuple(p.nodes), tuple(p.ports)): i for i, p in enumerate(rev_AB)
            }
            fwd_AB_idx: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], int] = {
                (tuple(p.nodes), tuple(p.ports)): i for i, p in enumerate(fwd_AB)
            }
            rev_BA_idx: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], int] = {
                (tuple(p.nodes), tuple(p.ports)): i for i, p in enumerate(rev_BA)
            }
            fwd_BA_idx: Dict[Tuple[Tuple[int, ...], Tuple[int, ...]], int] = {
                (tuple(p.nodes), tuple(p.ports)): i for i, p in enumerate(fwd_BA)
            }

            # ----- 写入 A→B 的主路径（生成 PST）-----
            f_ids_AB: List[int] = []
            for i, p in enumerate(fwd_AB):
                f_key = (tuple(p.nodes), tuple(p.ports))
                if f_key not in fwd_AB_to_rev_key:
                    mismatch_logs.append(
                        f"pair({src},{dst}) AB reverse missing for forward path"
                    )
                    continue

                r_key = fwd_AB_to_rev_key[f_key]
                if r_key not in rev_AB_idx:
                    mismatch_logs.append(
                        f"pair({src},{dst}) AB reverse key unresolved"
                    )
                    continue

                pid = base_fwd_AB + i
                reverse_pid = base_rev_AB + rev_AB_idx[r_key]

                f_ids_AB.append(pid)
                pit_writer.writerow(
                    [
                        pid,
                        p.path_length,
                        format_int_list(p.nodes),
                        format_int_list(p.ports),
                        reverse_pid,
                        f"{p.total_base_delay_ns:.3f}",
                        f"{p.weight:.3f}",
                    ]
                )
                total_paths += 1

            # ----- 写入 A→B 的反向路径（不生成 PST）-----
            for j, p in enumerate(rev_AB):
                r_key = (tuple(p.nodes), tuple(p.ports))
                f_key = next((fk for fk, rk in fwd_AB_to_rev_key.items() if rk == r_key), None)
                if f_key is None or f_key not in fwd_AB_idx:
                    continue

                pid = base_rev_AB + j
                reverse_pid = base_fwd_AB + fwd_AB_idx[f_key]

                pit_writer.writerow(
                    [
                        pid,
                        p.path_length,
                        format_int_list(p.nodes),
                        format_int_list(p.ports),
                        reverse_pid,
                        f"{p.total_base_delay_ns:.3f}",
                        f"{p.weight:.3f}",
                    ]
                )
                total_paths += 1

            # ----- 写入 B→A 的主路径（生成 PST）-----
            f_ids_BA: List[int] = []
            for i, p in enumerate(fwd_BA):
                f_key = (tuple(p.nodes), tuple(p.ports))
                if f_key not in fwd_BA_to_rev_key:
                    mismatch_logs.append(
                        f"pair({dst},{src}) BA reverse missing for forward path"
                    )
                    continue

                r_key = fwd_BA_to_rev_key[f_key]
                if r_key not in rev_BA_idx:
                    mismatch_logs.append(
                        f"pair({dst},{src}) BA reverse key unresolved"
                    )
                    continue

                pid = base_fwd_BA + i
                reverse_pid = base_rev_BA + rev_BA_idx[r_key]

                f_ids_BA.append(pid)
                pit_writer.writerow(
                    [
                        pid,
                        p.path_length,
                        format_int_list(p.nodes),
                        format_int_list(p.ports),
                        reverse_pid,
                        f"{p.total_base_delay_ns:.3f}",
                        f"{p.weight:.3f}",
                    ]
                )
                total_paths += 1

            # ----- 写入 B→A 的反向路径（不生成 PST）-----
            for j, p in enumerate(rev_BA):
                r_key = (tuple(p.nodes), tuple(p.ports))
                f_key = next((fk for fk, rk in fwd_BA_to_rev_key.items() if rk == r_key), None)
                if f_key is None or f_key not in fwd_BA_idx:
                    continue

                pid = base_rev_BA + j
                reverse_pid = base_fwd_BA + fwd_BA_idx[f_key]

                pit_writer.writerow(
                    [
                        pid,
                        p.path_length,
                        format_int_list(p.nodes),
                        format_int_list(p.ports),
                        reverse_pid,
                        f"{p.total_base_delay_ns:.3f}",
                        f"{p.weight:.3f}",
                    ]
                )
                total_paths += 1

            # 更新全局 path_id 游标。
            path_id_counter = base_rev_BA + len(rev_BA)

            # ----- 写入 PST（只记录主动发起的方向）-----
            pst_writer.writerow([src, dst, len(f_ids_AB), format_int_list(f_ids_AB)])
            pst_writer.writerow([dst, src, len(f_ids_BA), format_int_list(f_ids_BA)])

            if total_pairs % 20000 == 0:
                print(f"processed unordered pairs: {total_pairs}")

    print("=== UB Mesh PIT/PST generation done ===")
    print(f"case_dir: {case_dir}")
    print(f"hosts: {len(hosts)}")
    print(f"unordered host pairs processed: {total_pairs}")
    print(f"pit entries written: {total_paths}")
    print(f"pit file: {pit_csv}")
    print(f"pst file: {pst_csv}")
    print("[ScenarioStats]")
    for k in ("same_rack_same_col", "same_rack_diff_col", "same_pod_diff_rack", "cross_pod_placeholder"):
        print(f"  - {k}: {scenario_counter[k]}")

    if mismatch_logs:
        print("\n[Validation] template upper-bound deltas / reverse issues:")
        show = min(60, len(mismatch_logs))
        for line in mismatch_logs[:show]:
            print(f"  - {line}")
        if len(mismatch_logs) > show:
            print(f"  ... and {len(mismatch_logs) - show} more")
    else:
        print("\n[Validation] all template upper-bounds matched realized paths.")


# ... existing code ...

def main() -> None:
    """命令行入口。"""
    parser = argparse.ArgumentParser(description="Build UB Mesh path_index_table.csv and path_search_table.csv")
    parser.add_argument(
        "--case-dir",
        default="/app/ns-3-ub/scratch/tp_UB_Mesh_V0002",#4月1号下午，jyxiao改
        help="Directory containing node.csv and topology.csv",
    )
    args = parser.parse_args()

    write_tables(Path(args.case_dir))


if __name__ == "__main__":
    main()