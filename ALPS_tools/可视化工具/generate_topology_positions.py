#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
第一阶段：基于 topology.csv 生成 3D 拓扑节点坐标。

当前目标：
1) 读取拓扑 CSV 中出现的节点 ID
2) 按固定 Host-ToR-DU-L1-HRS 五层架构解码节点语义位置
3) 计算满足约束的 XYZ 坐标
4) 输出可直接给 Three.js 使用的 JSON

说明：
- 仅实现“节点摆放位置”能力
- 不涉及时间序列回放和指标切换
"""

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


# =========================
# 拓扑宏定义（与现有脚本保持一致）
# =========================
POD_NUM = 1
RACK_NUM = 4
RACK_ROWS = 8
RACK_COLS = 8
LRS_LAYERS = 4
DU_PER_LRS = 16
L1_PER_LRS = 4
HRS_NUM = 4


# =========================
# ID 范围定义
# =========================
HOST_COUNT = POD_NUM * RACK_NUM * RACK_ROWS * RACK_COLS
TOR_COUNT = POD_NUM * RACK_NUM * RACK_ROWS * RACK_COLS
DU_COUNT = POD_NUM * RACK_NUM * LRS_LAYERS * DU_PER_LRS
L1_COUNT = POD_NUM * RACK_NUM * LRS_LAYERS * L1_PER_LRS
HRS_COUNT = HRS_NUM

ID_RANGES = {
    "Host": (0, HOST_COUNT - 1),
    "ToR": (HOST_COUNT, HOST_COUNT + TOR_COUNT - 1),
    "DU": (HOST_COUNT + TOR_COUNT, HOST_COUNT + TOR_COUNT + DU_COUNT - 1),
    "L1": (
        HOST_COUNT + TOR_COUNT + DU_COUNT,
        HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT - 1,
    ),
    "HRS": (
        HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT,
        HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT + HRS_COUNT - 1,
    ),
}


# =========================
# 几何布局参数
# =========================
RACKS_PER_ROW = 4

# Host/ToR 8x8 网格的单元间距
HOST_DX = 1.0
HOST_DY = 1.0

# Rack 间距 = 一个 Rack 宽度
RACK_WIDTH_X = (RACK_COLS - 1) * HOST_DX
RACK_GAP_X = RACK_WIDTH_X
RACK_STEP_X = RACK_WIDTH_X + RACK_GAP_X

# 若 Rack 数超过 4，将换行布局
RACK_WIDTH_Y = (RACK_ROWS - 1) * HOST_DY
RACK_GAP_Y = RACK_WIDTH_Y
RACK_STEP_Y = RACK_WIDTH_Y + RACK_GAP_Y

# Rack 内部：DU / L1 在 Host 网格上方
DU_BASE_OFFSET_Y = 0.0  # 固定 Y 偏移
DU_LAYER_GAP_Y = 2.8
DU_ROW_WIDTH_X = RACK_WIDTH_X
DU_LAYER_SHIFT_X = 1.2

L1_BASE_OFFSET_Y = 0.0   # 固定 Y 偏移
L1_LAYER_GAP_Y = 2.8
L1_ROW_WIDTH_X = RACK_WIDTH_X
L1_LAYER_SHIFT_X = 1.2

HRS_OFFSET_Y = 3.0

# 5 层 Z 轴高度
Z_LEVELS = {
    "Host": 0.0,
    "ToR": 5.0,
    "DU": 10.0,
    "L1": 15.0,
    "HRS": 20.0,
}


def get_node_type(node_id: int) -> Optional[str]:
    for node_type, (start, end) in ID_RANGES.items():
        if start <= node_id <= end:
            return node_type
    return None


def decode_node_position(node_id: int) -> Optional[Dict[str, int]]:
    """按现有 UB Mesh 编号规则把 node_id 解码为语义坐标。"""
    node_type = get_node_type(node_id)

    if node_type == "Host":
        offset = node_id - ID_RANGES["Host"][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)
        remainder = offset % (RACK_NUM * RACK_ROWS * RACK_COLS)
        rack = remainder // (RACK_ROWS * RACK_COLS)
        remainder = remainder % (RACK_ROWS * RACK_COLS)
        # 编号规则：先列后行
        col = remainder // RACK_ROWS
        row = remainder % RACK_ROWS
        return {"pod": pod, "rack": rack, "row": row, "col": col}

    if node_type == "ToR":
        offset = node_id - ID_RANGES["ToR"][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)
        remainder = offset % (RACK_NUM * RACK_ROWS * RACK_COLS)
        rack = remainder // (RACK_ROWS * RACK_COLS)
        remainder = remainder % (RACK_ROWS * RACK_COLS)
        # 编号规则：先列后行
        col = remainder // RACK_ROWS
        row = remainder % RACK_ROWS
        return {"pod": pod, "rack": rack, "row": row, "col": col}

    if node_type == "DU":
        offset = node_id - ID_RANGES["DU"][0]
        pod = offset // (RACK_NUM * LRS_LAYERS * DU_PER_LRS)
        remainder = offset % (RACK_NUM * LRS_LAYERS * DU_PER_LRS)
        rack = remainder // (LRS_LAYERS * DU_PER_LRS)
        remainder = remainder % (LRS_LAYERS * DU_PER_LRS)
        layer = remainder // DU_PER_LRS
        du_idx = remainder % DU_PER_LRS
        return {"pod": pod, "rack": rack, "layer": layer, "du_idx": du_idx}

    if node_type == "L1":
        offset = node_id - ID_RANGES["L1"][0]
        pod = offset // (RACK_NUM * LRS_LAYERS * L1_PER_LRS)
        remainder = offset % (RACK_NUM * LRS_LAYERS * L1_PER_LRS)
        rack = remainder // (LRS_LAYERS * L1_PER_LRS)
        remainder = remainder % (LRS_LAYERS * L1_PER_LRS)
        layer = remainder // L1_PER_LRS
        l1_idx = remainder % L1_PER_LRS
        return {"pod": pod, "rack": rack, "layer": layer, "l1_idx": l1_idx}

    if node_type == "HRS":
        hrs_id = node_id - ID_RANGES["HRS"][0]
        return {"hrs_id": hrs_id}

    return None


def rack_origin_xy(rack: int) -> Tuple[float, float]:
    rack_col = rack % RACKS_PER_ROW
    rack_row = rack // RACKS_PER_ROW
    x0 = rack_col * RACK_STEP_X
    y0 = rack_row * RACK_STEP_Y
    return x0, y0


def compute_xyz(node_id: int, decoded: Dict[str, int], du_layer_centers: Dict[int, Tuple[float, float]]) -> Tuple[float, float, float]:
    node_type = get_node_type(node_id)
    if node_type is None:
        raise ValueError("invalid node id: {}".format(node_id))

    if node_type in ("Host", "ToR"):
        x0, y0 = rack_origin_xy(decoded["rack"])
        x = x0 + decoded["col"] * HOST_DX
        y = y0 + decoded["row"] * HOST_DY
        z = Z_LEVELS[node_type]
        return x, y, z

    # =========================
    # ✅ 改动 1：DU 层改为在 rack 内部垂直堆叠，不再横向偏移
    # 原因：避免不同 layer 的 DU 在 X 上错开，形成“扇形”
    # 现在所有 DU layer 都在相同 X 区域内，仅靠 Y 分层
    # =========================
    if node_type == "DU":
        x0, y0 = rack_origin_xy(decoded["rack"])
        # 使用 DU_ROW_WIDTH_X 控制宽度，但不再随 layer 偏移
        x = x0 + (decoded["du_idx"] / float(DU_PER_LRS - 1)) * DU_ROW_WIDTH_X
        # Y 轴按 layer 分层，每层之间留出间隙
        y = y0 + DU_BASE_OFFSET_Y + decoded["layer"] * DU_LAYER_GAP_Y
        z = Z_LEVELS["DU"]
        return x, y, z

    # =========================
    # ✅ 改动 2：L1 层同样改为垂直堆叠，不横向偏移
    # 原因：保持与 DU 一致的视觉风格，避免“阶梯状”错位
    # =========================
    if node_type == "L1":
        x0, y0 = rack_origin_xy(decoded["rack"])
        # L1 节点在 X 上均匀分布，不随 layer 偏移
        x = x0 + (decoded["l1_idx"] / float(L1_PER_LRS - 1)) * L1_ROW_WIDTH_X
        # Y 轴按 layer 分层
        y = y0 + L1_BASE_OFFSET_Y + decoded["layer"] * L1_LAYER_GAP_Y
        z = Z_LEVELS["L1"]
        return x, y, z

    # =========================
    # ✅ 改动 3：HRS 节点均匀分布在顶部，且与 DU layer 一一对应
    # 原因：原设计中 HRS 的 X 取 DU 中心，但 Y 不统一，导致“斜拉”
    # 现在让 HRS 的 4 个节点在 X 上均匀分布，Y 统一抬高
    # =========================
    if node_type == "HRS":
        hrs_id = decoded["hrs_id"]
        if hrs_id not in du_layer_centers:
            raise ValueError("missing DU center for layer {}".format(hrs_id))
        x = du_layer_centers[hrs_id][0]
        # ✅ Y 坐标按 layer 分布，不再是统一抬高
        y = DU_BASE_OFFSET_Y + hrs_id * DU_LAYER_GAP_Y  # 每个 HRS 在 Y 上偏移 1.0
        z = Z_LEVELS["HRS"]
        return x, y, z

    raise ValueError("unsupported node type: {}".format(node_type))

def read_node_ids_from_topology(topology_csv: Path) -> Set[int]:
    node_ids = set()
    with topology_csv.open("r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            node_ids.add(int(row["nodeId1"]))
            node_ids.add(int(row["nodeId2"]))
    return node_ids


def compute_du_layer_centers(all_node_ids: Set[int]) -> Dict[int, Tuple[float, float]]:
    layer_points = defaultdict(list)
    for node_id in all_node_ids:
        if get_node_type(node_id) != "DU":
            continue
        decoded = decode_node_position(node_id)
        if decoded is None:
            continue
        x0, y0 = rack_origin_xy(decoded["rack"])
        # ✅ 去掉 DU_LAYER_SHIFT_X
        x = x0 + (decoded["du_idx"] / float(DU_PER_LRS - 1)) * DU_ROW_WIDTH_X
        y = y0 + DU_BASE_OFFSET_Y + decoded["layer"] * DU_LAYER_GAP_Y
        layer_points[decoded["layer"]].append((x, y))

    centers = {}
    for layer in range(LRS_LAYERS):
        points = layer_points.get(layer, [])
        if not points:
            continue
        sum_x = sum(p[0] for p in points)
        sum_y = sum(p[1] for p in points)
        centers[layer] = (sum_x / len(points), sum_y / len(points))
    return centers


def build_layout(topology_csv: Path) -> Dict:
    node_ids = read_node_ids_from_topology(topology_csv)
    du_layer_centers = compute_du_layer_centers(node_ids)

    nodes = []
    for node_id in sorted(node_ids):
        node_type = get_node_type(node_id)
        if node_type is None:
            raise ValueError("节点 ID 超出已知范围: {}".format(node_id))

        decoded = decode_node_position(node_id)
        if decoded is None:
            raise ValueError("节点 ID 无法解码: {}".format(node_id))

        x, y, z = compute_xyz(node_id, decoded, du_layer_centers)
        nodes.append(
            {
                "id": node_id,
                "type": node_type,
                "x": round(x, 4),
                "y": round(y, 4),
                "z": round(z, 4),
                "decoded": decoded,
            }
        )

    edges = []
    with topology_csv.open("r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            edges.append(
                {
                    "src": int(row["nodeId1"]),
                    "dst": int(row["nodeId2"]),
                    "srcPort": int(row["portId1"]),
                    "dstPort": int(row["portId2"]),
                    "bandwidth": row["bandwidth"],
                    "delay": row["delay"],
                }
            )

    return {
        "meta": {
            "source": str(topology_csv),
            "architecture": "Host-ToR-DU-L1-HRS",
            "nodeCount": len(nodes),
            "edgeCount": len(edges),
            "zLevels": Z_LEVELS,
            "layoutRules": {
                "racksPerRow": RACKS_PER_ROW,
                "rackGapEqualsRackWidth": True,
                "hostTorAlignedXY": True,
                "hostTorGrid": "8x8",
                "duPerRackPerLayer": "1x16",
                "l1PerRackPerLayer": "1x4",
                "hrs": "1x4 horizontal, X-aligned to DU layer centers",
            },
        },
        "nodes": nodes,
        "edges": edges,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="生成五层拓扑节点 3D 坐标 JSON")
    parser.add_argument(
        "-i",
        "--input",
        required=True,
        help="输入 topology.csv 文件路径",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="",
        help="输出 JSON 路径（默认与输入同目录: topology_layout.json）",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    input_path = Path(args.input).resolve()
    if not input_path.exists():
        raise FileNotFoundError("输入文件不存在: {}".format(input_path))

    if args.output:
        output_path = Path(args.output).resolve()
    else:
        output_path = Path(__file__).resolve().parent / "topology_layout.json"

    layout = build_layout(input_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as f:
        json.dump(layout, f, ensure_ascii=False, indent=2)

    print("[OK] 位置文件已生成: {}".format(output_path))
    print("[INFO] 节点数: {}, 链路数: {}".format(layout["meta"]["nodeCount"], layout["meta"]["edgeCount"]))


if __name__ == "__main__":
    main()
