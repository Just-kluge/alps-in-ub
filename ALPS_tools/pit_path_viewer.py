#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
UB Mesh PIT 路径查询工具

功能：
- 通过两个 Host 的位置坐标查询它们之间的所有可用路径
- 显示每条路径的详细节点序列、类型、位置和输出端口
- 支持按 Layer 筛选路径（跨列/跨 Rack 通信时）
- 自动识别通信场景（同列/同 Rack 跨列/跨 Rack）
"""

import csv
from pathlib import Path
from collections import defaultdict
from typing import Dict, List, Optional, Tuple

# =========================
# Topology macros (必须与拓扑生成脚本一致)
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
# ID 范围定义 (与生成脚本严格一致)
# =========================

HOST_COUNT = POD_NUM * RACK_NUM * RACK_ROWS * RACK_COLS  # 256
TOR_COUNT = POD_NUM * RACK_NUM * RACK_ROWS * RACK_COLS   # 256
DU_COUNT = POD_NUM * RACK_NUM * LRS_LAYERS * DU_PER_LRS  # 256
L1_COUNT = POD_NUM * RACK_NUM * LRS_LAYERS * L1_PER_LRS  # 64
HRS_COUNT = HRS_NUM                                       # 4

ID_RANGES = {
    'Host': (0, HOST_COUNT - 1),
    'ToR': (HOST_COUNT, HOST_COUNT + TOR_COUNT - 1),
    'DU': (HOST_COUNT + TOR_COUNT, HOST_COUNT + TOR_COUNT + DU_COUNT - 1),
    'L1': (HOST_COUNT + TOR_COUNT + DU_COUNT, HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT - 1),
    'HRS': (HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT, HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT + HRS_COUNT - 1),
}

PIT_FILE = "/app/ns-3-ub/scratch/tp_UB_Mesh_V0001/path_index_table.csv"
PST_FILE = "/app/ns-3-ub/scratch/tp_UB_Mesh_V0001/path_search_table.csv"


class Colors:
    """ANSI 颜色代码定义"""
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    MAGENTA = '\033[95m'
    CYAN = '\033[96m'
    WHITE = '\033[97m'
    BOLD = '\033[1m'
    RESET = '\033[0m'


def colorize(text: str, color: str) -> str:
    """给文本添加颜色"""
    return f"{color}{text}{Colors.RESET}"


def get_node_type(node_id: int) -> Optional[str]:
    """根据 ID 判断节点类型"""
    for node_type, (start, end) in ID_RANGES.items():
        if start <= node_id <= end:
            return node_type
    return None


def host_id_from_location(rack: int, row: int, col: int, pod: int = 0) -> int:
    """根据位置坐标计算 Host节点 ID"""
    hosts_per_rack = RACK_ROWS * RACK_COLS
    racks_per_pod = RACK_NUM

# 将用户输入的 1-based 坐标转换为 0-based 索引
    rack_idx = rack - 1
    row_idx = row - 1
    col_idx = col - 1

# 新拓扑采用"先列后行"编号：in_rack_idx = col * RACK_ROWS + row
    in_rack_idx = col_idx * RACK_ROWS + row_idx
    return pod * racks_per_pod * hosts_per_rack + rack_idx * hosts_per_rack + in_rack_idx


def get_node_position(node_id: int) -> Optional[Dict]:
    """根据 ID 计算节点位置坐标"""
    node_type = get_node_type(node_id)
    
    if node_type == 'Host':
        offset = node_id - ID_RANGES['Host'][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)


def get_node_position(node_id: int) -> Optional[Dict]:
    """根据 ID 计算节点位置坐标"""
    node_type = get_node_type(node_id)
    
    if node_type == 'Host':
        offset = node_id - ID_RANGES['Host'][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)
        remainder = offset % (RACK_NUM * RACK_ROWS * RACK_COLS)
        rack = remainder // (RACK_ROWS * RACK_COLS)
        remainder = remainder % (RACK_ROWS * RACK_COLS)
           # 新拓扑：先列后行
        col = remainder // RACK_ROWS
        row = remainder % RACK_ROWS
        return {'pod': pod, 'rack': rack, 'row': row, 'col': col}
    
    elif node_type == 'ToR':
        offset = node_id - ID_RANGES['ToR'][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)
        remainder = offset % (RACK_NUM * RACK_ROWS * RACK_COLS)
        rack = remainder // (RACK_ROWS * RACK_COLS)
        remainder = remainder % (RACK_ROWS * RACK_COLS)
         # 新拓扑：先列后行
        col = remainder // RACK_ROWS
        row = remainder % RACK_ROWS
        return {'pod': pod, 'rack': rack, 'row': row, 'col': col}
    
    elif node_type == 'DU':
        offset = node_id - ID_RANGES['DU'][0]
        pod = offset // (RACK_NUM * LRS_LAYERS * DU_PER_LRS)
        remainder = offset % (RACK_NUM * LRS_LAYERS * DU_PER_LRS)
        rack = remainder // (LRS_LAYERS * DU_PER_LRS)
        remainder = remainder % (LRS_LAYERS * DU_PER_LRS)
        layer = remainder // DU_PER_LRS
        du_idx = remainder % DU_PER_LRS
        return {'pod': pod, 'rack': rack, 'layer': layer, 'du_idx': du_idx}
    
    elif node_type == 'L1':
        offset = node_id - ID_RANGES['L1'][0]
        pod = offset // (RACK_NUM * LRS_LAYERS * L1_PER_LRS)
        remainder = offset % (RACK_NUM * LRS_LAYERS * L1_PER_LRS)
        rack = remainder // (LRS_LAYERS * L1_PER_LRS)
        remainder = remainder % (LRS_LAYERS * L1_PER_LRS)
        layer = remainder // L1_PER_LRS
        l1_idx = remainder % L1_PER_LRS
        return {'pod': pod, 'rack': rack, 'layer': layer, 'l1_idx': l1_idx}
    
    elif node_type == 'HRS':
        hrs_id = node_id - ID_RANGES['HRS'][0]
        return {'hrs_id': hrs_id}
    
    return None


def format_position(node_type: str, position: Dict) -> str:
    """格式化位置信息为可读字符串"""
    if position is None:
        return "未知位置"
    
    if node_type == 'Host' or node_type == 'ToR':
        return f"Rack-{position['rack']+1}的第{position['row']+1}行第{position['col']+1}列"
    
    elif node_type == 'DU':
        return f"Rack-{position['rack']+1} Layer-{position['layer']+1}的第{position['du_idx']+1}号 DU"
    
    elif node_type == 'L1':
        return f"Rack-{position['rack']+1} Layer-{position['layer']+1}的第{position['l1_idx']+1}号 L1"
    
    elif node_type == 'HRS':
        return f"HRS-[{position['hrs_id']+1}]"
    
    return "未知位置"


def load_pit_table(filepath: Path) -> Dict[int, Dict]:
    """加载 PIT 表"""
    pit_entries = {}
    
    if not filepath.exists():
        print(f"❌ 错误：PIT 文件不存在：{filepath}")
        return None
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                path_id = int(row['path_id'])
                pit_entries[path_id] = {
                    'path_id': path_id,
                    'path_length': int(row['path_length']),
                    'traverse_nodes': list(map(int, row['traverse_nodes'].split())),
                    'output_ports': list(map(int, row['output_ports'].split())),
                    'reverse_path_id': int(row['reverse_path_id']),
                    'total_base_delay_ns': float(row['total_base_delay_ns'])
                }
        
        return pit_entries
    
    except Exception as e:
        print(f"❌ 错误：读取 PIT 文件失败：{e}")
        return None


def load_pst_table(filepath: Path) -> Dict[Tuple[int, int], List[int]]:
    """加载 PST 表，返回 (src, dst) -> [path_ids] 的映射"""
    pst_entries = {}
    
    if not filepath.exists():
        print(f"❌ 错误：PST 文件不存在：{filepath}")
        return None
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                src = int(row['sourceNode'])
                dst = int(row['destNode'])
                num_paths = int(row['num_paths'])
                path_ids = list(map(int, row['path_ids'].split())) if num_paths > 0 else []
                
                pst_entries[(src, dst)] = path_ids
        
        return pst_entries
    
    except Exception as e:
        print(f"❌ 错误：读取 PST 文件失败：{e}")
        return None


def determine_communication_scenario(src_pos: Dict, dst_pos: Dict) -> str:
    """判断通信场景"""
    same_rack = (src_pos['rack'] == dst_pos['rack'])
    same_col = (src_pos['col'] == dst_pos['col'])
    
    if same_rack and same_col:
        return "same_column"
    elif same_rack:
        return "same_rack_diff_column"
    else:
        return "cross_rack"


def get_layer_from_du_node(du_node_id: int) -> int:
    """从 DU 节点 ID 获取 Layer 索引"""
    pos = get_node_position(du_node_id)
    if pos and 'layer' in pos:
        return pos['layer']
    return -1


def filter_paths_by_layer(paths: List[Dict], pit_entries: Dict, target_layer: Optional[int]) -> List[Dict]:
    """根据 Layer 筛选路径"""
    if target_layer is None:
        return paths
    
    filtered = []
    for path_entry in paths:
        path_id = path_entry['path_id']
        pit_entry = pit_entries.get(path_id)
        if not pit_entry:
            continue
        
        # 检查路径中是否有 DU 节点属于目标 Layer
        has_target_layer = False
        for node_id in pit_entry['traverse_nodes']:
            node_type = get_node_type(node_id)
            if node_type == 'DU':
                du_layer = get_layer_from_du_node(node_id)
                if du_layer == target_layer:
                    has_target_layer = True
                    break
        
        if has_target_layer:
            filtered.append(path_entry)
    
    return filtered


def group_paths_by_layer(paths: List[Dict], pit_entries: Dict) -> Dict[int, List[Dict]]:
    """将路径按 Layer 分组"""
    groups = defaultdict(list)
    
    for path_entry in paths:
        path_id = path_entry['path_id']
        pit_entry = pit_entries.get(path_id)
        if not pit_entry:
            continue
        
        # 找到路径中的第一个 DU 节点来确定 Layer
        for node_id in pit_entry['traverse_nodes']:
            node_type = get_node_type(node_id)
            if node_type == 'DU':
                du_layer = get_layer_from_du_node(node_id)
                if du_layer >= 0:
                    groups[du_layer].append(path_entry)
                    break
    
    return dict(groups)


def display_path_details(path_entry: Dict, pit_entry: Dict, show_header: bool = True):
    """显示单条路径的详细信息"""
    if show_header:
        print(f"\n[Path ID: {pit_entry['path_id']}] Length: {pit_entry['path_length']}, "
              f"Delay: {pit_entry['total_base_delay_ns']:.3f}ns")
    
    print("  节点序列及转发端口:")
    
    nodes = pit_entry['traverse_nodes']
    ports = pit_entry['output_ports']
    
    for i, (node_id, port_id) in enumerate(zip(nodes, ports)):
        node_type = get_node_type(node_id)
        position = get_node_position(node_id)
        pos_str = format_position(node_type, position)
        
        # 格式化输出：对齐不同类型的节点
        type_padding = 6 - len(node_type)
        type_str = f"{node_type}{' ' * type_padding}"
        
        if i < len(nodes) - 1:
            print(f"    [{node_id}] {type_str} @{pos_str:<35} → Port {port_id}")
        else:
            print(f"    [{node_id}] {type_str} @{pos_str:<35}")


def main():
    """主函数"""
    print("\n" + "="*80)
    print(colorize("UB Mesh PIT 路径查询工具", Colors.BOLD))
    print("="*80)
    print(f"PIT 文件：{PIT_FILE}")
    print(f"PST 文件：{PST_FILE}")
    print("="*80 + "\n")
    
    # 加载 PIT 和 PST 表
    pit_entries = load_pit_table(Path(PIT_FILE))
    pst_entries = load_pst_table(Path(PST_FILE))
    
    if pit_entries is None or pst_entries is None:
        return
    
    print(f"✅ 成功加载 {len(pit_entries)} 条 PIT 记录")
    print(f"✅ 成功加载 {len(pst_entries)} 条 PST 记录\n")
    
    # 交互查询循环
    while True:
        try:
            print("\n" + "-"*80)
            print("请输入 Host 坐标 (格式：rack row col)，输入 q 退出")
            print("说明：坐标从 1 开始计数，例如 '1 1 1' 表示 Rack-1 的第 1 行第 1 列")
            print("-"*80)
            
            # 输入源节点
            src_input = input("请输入源 Host 的坐标 (rack row col): ").strip()
            if src_input.lower() == 'q':
                print(colorize("👋 感谢使用，再见！", Colors.GREEN))
                break
            
            src_parts = list(map(int, src_input.split()))
            if len(src_parts) != 3:
                print(colorize("❌ 错误：请输入 3 个整数 (rack row col)", Colors.RED))
                continue
            
            src_rack, src_row, src_col = src_parts
            src_host_id = host_id_from_location(src_rack, src_row, src_col)
            src_pos = get_node_position(src_host_id)
            
            # 输入目的节点
            dst_input = input("请输入目的 Host 的坐标 (rack row col): ").strip()
            if dst_input.lower() == 'q':
                print(colorize("👋 感谢使用，再见！", Colors.GREEN))
                break
            
            dst_parts = list(map(int, dst_input.split()))
            if len(dst_parts) != 3:
                print(colorize("❌ 错误：请输入 3 个整数 (rack row col)", Colors.RED))
                continue
            
            dst_rack, dst_row, dst_col = dst_parts
             # 验证坐标范围（1-based）
            if not (1 <= dst_rack <= RACK_NUM and 1 <= dst_row <= RACK_ROWS and 1 <= dst_col <= RACK_COLS):
                print(colorize(f"❌ 错误：坐标超出范围 (rack:1-{RACK_NUM}, row:1-{RACK_ROWS}, col:1-{RACK_COLS})", Colors.RED))
                continue
            
            dst_host_id = host_id_from_location(dst_rack, dst_row, dst_col)
            dst_pos = get_node_position(dst_host_id)
            
            # 检查是否是同一个 Host
            if src_host_id == dst_host_id:
                print(colorize("❌ 错误：源 Host 和目的 Host 不能相同", Colors.RED))
                continue
            
            # 判断通信场景
            scenario = determine_communication_scenario(src_pos, dst_pos)
            
            # 获取 PST 中的路径 IDs
            pst_key = (src_host_id, dst_host_id)
            if pst_key not in pst_entries:
                print(colorize(f"❌ 错误：未找到从 Host-{src_host_id} 到 Host-{dst_host_id} 的路径", Colors.RED))
                continue
            
            path_ids = pst_entries[pst_key]
            if not path_ids:
                print(colorize(f"❌ 错误：Host-{src_host_id} 到 Host-{dst_host_id} 没有可用路径", Colors.RED))
                continue
            
            # 构建路径条目列表
            all_paths = []
            for pid in path_ids:
                if pid in pit_entries:
                    all_paths.append({
                        'path_id': pid,
                        'pit_entry': pit_entries[pid]
                    })
            
            # 按 path_id 排序
            all_paths.sort(key=lambda x: x['path_id'])
            
            # 显示源和目的信息
            print("\n" + "="*80)
            print(colorize("源 Host 信息:", Colors.BOLD))
            print(f"  节点 ID: {src_host_id}")
            print(f"  类型: Host")
            print(f"  位置: {format_position('Host', src_pos)} (pod=0, rack={src_rack}, row={src_row}, col={src_col})")
            
            print("\n" + colorize("目的 Host 信息:", Colors.BOLD))
            print(f"  节点 ID: {dst_host_id}")
            print(f"  类型: Host")
            print(f"  位置: {format_position('Host', dst_pos)} (pod=0, rack={dst_rack}, row={dst_row}, col={dst_col})")
            
            # 根据场景处理
            if scenario == "same_column":
                # 同列通信：不需要选择 Layer
                print("\n" + "="*80)
                print(colorize("检测到同列通信，不需要选择 Layer。", Colors.GREEN))
                print(f"共找到 {len(all_paths)} 条路径")
                print("="*80)
                
                for i, path_data in enumerate(all_paths):
                    display_path_details(path_data, path_data['pit_entry'])
            
            else:
                # 跨列或跨 Rack 通信：询问 Layer 选择
                print("\n" + "="*80)
                if scenario == "same_rack_diff_column":
                    print(colorize("检测到同 Rack 跨列通信。", Colors.YELLOW))
                else:
                    print(colorize("检测到跨 Rack 通信。", Colors.MAGENTA))
                
                layer_input = input("请选择 Layer (0-3)，直接回车表示显示所有 Layer: ").strip()
                
                if layer_input == "":
                    # 显示所有 Layer
                    print(f"\n共找到 {len(all_paths)} 条路径")
                    print("="*80)
                    
                    # 按 Layer 分组
                    layer_groups = group_paths_by_layer(
                        [{'path_id': p['path_id']} for p in all_paths],
                        pit_entries
                    )
                    
                    for layer in sorted(layer_groups.keys()):
                        layer_paths = layer_groups[layer]
                        print(f"\n{colorize(f'--- Layer-{layer} 路径 ({len(layer_paths)} 条) ---', Colors.CYAN)}")
                        
                        for path_data in all_paths:
                            if path_data['path_id'] in [p['path_id'] for p in layer_paths]:
                                display_path_details(path_data, path_data['pit_entry'])
                
                else:
                    # 显示指定 Layer
                    try:
                        target_layer = int(layer_input)
                        if target_layer < 0 or target_layer > 3:
                            print(colorize("❌ 错误：Layer 必须在 0-3 之间", Colors.RED))
                            continue
                        
                        filtered_paths = filter_paths_by_layer(
                            [{'path_id': p['path_id']} for p in all_paths],
                            pit_entries,
                            target_layer
                        )
                        
                        print(f"\n共找到 {len(filtered_paths)} 条路径 (Layer-{target_layer})")
                        print("="*80)
                        
                        for path_data in all_paths:
                            if path_data['path_id'] in [p['path_id'] for p in filtered_paths]:
                                display_path_details(path_data, path_data['pit_entry'])
                    
                    except ValueError:
                        print(colorize("❌ 错误：请输入有效的 Layer 编号 (0-3)", Colors.RED))
                        continue
            
            print("\n" + "="*80)
        
        except ValueError as e:
            print(colorize(f"❌ 错误：输入无效，{e}", Colors.RED))
        except KeyboardInterrupt:
            print(colorize("\n\n👋 程序中断，再见！", Colors.GREEN))
            break
        except Exception as e:
            print(colorize(f"❌ 发生未知错误：{e}", Colors.RED))


if __name__ == "__main__":
    main()