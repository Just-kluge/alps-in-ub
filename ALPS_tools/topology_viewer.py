#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
拓扑结构查询工具

功能：
- 通过节点 ID 查询节点信息和连接详情
- 显示节点类型、位置、所有连接关系
- 分组展示下行、Mesh、上行链路
"""

import os
import sys
from pathlib import Path
from collections import defaultdict

# =========================
# Topology macros (必须与 user_topo_pod_lrs_hrs_256NPU.py 一致)
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

TOPOLOGY_FILE = "/app/ns-3-ub/scratch/tp_UB_Mesh_V0001/topology.csv"


def get_node_type(node_id):
    """根据 ID 判断节点类型"""
    for node_type, (start, end) in ID_RANGES.items():
        if start <= node_id <= end:
            return node_type
    return None


def get_node_position(node_id):
    """根据 ID 计算节点位置坐标"""
    node_type = get_node_type(node_id)
    
    if node_type == 'Host':
        offset = node_id - ID_RANGES['Host'][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)
        remainder = offset % (RACK_NUM * RACK_ROWS * RACK_COLS)
        rack = remainder // (RACK_ROWS * RACK_COLS)
        remainder = remainder % (RACK_ROWS * RACK_COLS)
        col = remainder // RACK_COLS
        row = remainder % RACK_COLS
        return {'pod': pod, 'rack': rack, 'row': row, 'col': col}
    
    elif node_type == 'ToR':
        offset = node_id - ID_RANGES['ToR'][0]
        pod = offset // (RACK_NUM * RACK_ROWS * RACK_COLS)
        remainder = offset % (RACK_NUM * RACK_ROWS * RACK_COLS)
        rack = remainder // (RACK_ROWS * RACK_COLS)
        remainder = remainder % (RACK_ROWS * RACK_COLS)
        col = remainder // RACK_COLS
        row = remainder % RACK_COLS
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


def format_position(node_type, position):
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


def load_topology(filepath):
    """加载拓扑文件，构建邻接表"""
    adjacency_list = defaultdict(list)
    
    if not os.path.exists(filepath):
        print(f"❌ 错误：拓扑文件不存在：{filepath}")
        return None
    
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        # 检查是否有表头
        start_line = 0
        if len(lines) > 0 and 'nodeId1' in lines[0]:
            start_line = 1
        
        for line_num, line in enumerate(lines[start_line:], start=start_line+1):
            line = line.strip()
            if not line:
                continue
            
            parts = line.split(',')
            if len(parts) < 6:
                print(f"⚠️  警告：第{line_num}行格式不正确，跳过")
                continue
            
            try:
                node1_id = int(parts[0])
                port1_id = int(parts[1])
                node2_id = int(parts[2])
                port2_id = int(parts[3])
                bandwidth = parts[4]
                delay = parts[5]
                
                # 无向图，添加双向连接
                adjacency_list[node1_id].append({
                    'neighbor_id': node2_id,
                    'port_id': port1_id,
                    'bandwidth': bandwidth,
                    'delay': delay
                })
                
                adjacency_list[node2_id].append({
                    'neighbor_id': node1_id,
                    'port_id': port2_id,
                    'bandwidth': bandwidth,
                    'delay': delay
                })
                
            except ValueError as e:
                print(f"⚠️  警告：第{line_num}行解析失败：{e}")
                continue
        
        return adjacency_list
    
    except Exception as e:
        print(f"❌ 错误：读取拓扑文件失败：{e}")
        return None


def classify_connections(node_id, connections):
    """
    将连接按类型分组：下行、Mesh、上行
    
    分类规则：
    - Host: 只有上行 (到 ToR)
    - ToR: 下行 (到 Host), Mesh (到 ToR), 上行 (到 DU)
    - DU: 下行 (到 ToR), L1 连接 (到 L1), 上行 (到 HRS)
    - L1: 下行 (到 DU)
    - HRS: 下行 (到 DU)
    """
    node_type = get_node_type(node_id)
    
    groups = {
        'downlink': [],      # 下行链路
        'mesh': [],          # Mesh 链路（同类型互连）
        'uplink': [],        # 上行链路
        'l1_connection': []  # L1 专用连接
    }
    
    for conn in connections:
        neighbor_id = conn['neighbor_id']
        neighbor_type = get_node_type(neighbor_id)
        
        if node_type == 'Host':
            # Host 只有上行到 ToR
            if neighbor_type == 'ToR':
                groups['uplink'].append(conn)
        
        elif node_type == 'ToR':
            # ToR: 下行到 Host, Mesh 到 ToR, 上行到 DU
            if neighbor_type == 'Host':
                groups['downlink'].append(conn)
            elif neighbor_type == 'ToR':
                groups['mesh'].append(conn)
            elif neighbor_type == 'DU':
                groups['uplink'].append(conn)
        
        elif node_type == 'DU':
            # DU: 下行到 ToR, L1 连接到 L1, 上行到 HRS
            if neighbor_type == 'ToR':
                groups['downlink'].append(conn)
            elif neighbor_type == 'L1':
                groups['l1_connection'].append(conn)
            elif neighbor_type == 'HRS':
                groups['uplink'].append(conn)
        
        elif node_type == 'L1':
            # L1: 只有下行到 DU
            if neighbor_type == 'DU':
                groups['downlink'].append(conn)
        
        elif node_type == 'HRS':
            # HRS: 只有下行到 DU
            if neighbor_type == 'DU':
                groups['downlink'].append(conn)
    
    return groups


# ... existing code ...

def explain_id_calculation(node_type, position):
    """
    解释节点 ID 的计算公式和过程
    
    参数:
        node_type: 节点类型 (Host/ToR/DU/L1/HRS)
        position: 位置字典 (包含 rack, row, col, layer, du_idx, l1_idx 等)
    
    返回:
        字符串，描述 ID 计算公式（带颜色标记）
    """
    if node_type == 'Host':
        rack_val = position['rack']
        row_val = position['row']
        col_val = position['col']
        
        formula = (
            f"ID = {colorize('rack', Colors.YELLOW)} × ({colorize('行', Colors.BLUE)}×{colorize('列', Colors.BLUE)}) + "
            f"{colorize('row', Colors.YELLOW)} × {colorize('列', Colors.BLUE)} + {colorize('col', Colors.YELLOW)}\n"
            f"   = {colorize(str(rack_val), Colors.YELLOW)} × ({RACK_ROWS}×{RACK_COLS}) + "
            f"{colorize(str(row_val), Colors.YELLOW)} × {RACK_COLS} + {colorize(str(col_val), Colors.YELLOW)}\n"
            f"   = {colorize(str(rack_val), Colors.BLUE)} × {RACK_ROWS*RACK_COLS} + "
            f"{colorize(str(row_val*RACK_COLS + col_val), Colors.BLUE)}\n"
            f"   = {colorize(str(rack_val * RACK_ROWS * RACK_COLS + row_val * RACK_COLS + col_val), Colors.BLUE)}"
        )
        return formula
    
    elif node_type == 'ToR':
        base_id = HOST_COUNT
        rack_val = position['rack']
        row_val = position['row']
        col_val = position['col']
        offset = rack_val * (RACK_ROWS * RACK_COLS) + row_val * RACK_COLS + col_val
        
        formula = (
            f"ID = {colorize('Host 总数', Colors.BLUE)} + "
            f"{colorize('rack', Colors.YELLOW)} × ({colorize('行', Colors.BLUE)}×{colorize('列', Colors.BLUE)}) + "
            f"{colorize('row', Colors.YELLOW)} × {colorize('列', Colors.BLUE)} + {colorize('col', Colors.YELLOW)}\n"
            f"   = {colorize(str(HOST_COUNT), Colors.BLUE)} + "
            f"{colorize(str(rack_val), Colors.YELLOW)} × ({RACK_ROWS}×{RACK_COLS}) + "
            f"{colorize(str(row_val), Colors.YELLOW)} × {RACK_COLS} + {colorize(str(col_val), Colors.YELLOW)}\n"
            f"   = {colorize(str(HOST_COUNT), Colors.BLUE)} + "
            f"{colorize(str(rack_val), Colors.BLUE)} × {RACK_ROWS*RACK_COLS} + "
            f"{colorize(str(row_val*RACK_COLS + col_val), Colors.BLUE)}\n"
            f"   = {colorize(str(HOST_COUNT), Colors.BLUE)} + {colorize(str(offset), Colors.BLUE)}\n"
            f"   = {colorize(str(base_id + offset), Colors.BLUE)}"
        )
        return formula
    
    elif node_type == 'DU':
        base_id = HOST_COUNT + TOR_COUNT
        rack_val = position['rack']
        layer_val = position['layer']
        du_idx_val = position['du_idx']
        offset = rack_val * (LRS_LAYERS * DU_PER_LRS) + layer_val * DU_PER_LRS + du_idx_val
        
        formula = (
            f"ID = ({colorize('Host+ToR 总数', Colors.BLUE)}) + "
            f"{colorize('rack', Colors.YELLOW)} × ({colorize('层数', Colors.BLUE)}×{colorize('每层 DU 数', Colors.BLUE)}) + "
            f"{colorize('layer', Colors.YELLOW)} × {colorize('每层 DU 数', Colors.BLUE)} + {colorize('du_idx', Colors.YELLOW)}\n"
            f"   = ({HOST_COUNT}+{TOR_COUNT}) + "
            f"{colorize(str(rack_val), Colors.YELLOW)} × ({LRS_LAYERS}×{DU_PER_LRS}) + "
            f"{colorize(str(layer_val), Colors.YELLOW)} × {DU_PER_LRS} + {colorize(str(du_idx_val), Colors.YELLOW)}\n"
            f"   = {colorize(str(HOST_COUNT+TOR_COUNT), Colors.BLUE)} + "
            f"{colorize(str(rack_val), Colors.BLUE)} × {LRS_LAYERS*DU_PER_LRS} + "
            f"{colorize(str(layer_val*DU_PER_LRS + du_idx_val), Colors.BLUE)}\n"
            f"   = {colorize(str(HOST_COUNT+TOR_COUNT), Colors.BLUE)} + {colorize(str(offset), Colors.BLUE)}\n"
            f"   = {colorize(str(base_id + offset), Colors.BLUE)}"
        )
        return formula
    
    elif node_type == 'L1':
        base_id = HOST_COUNT + TOR_COUNT + DU_COUNT
        rack_val = position['rack']
        layer_val = position['layer']
        l1_idx_val = position['l1_idx']
        offset = rack_val * (LRS_LAYERS * L1_PER_LRS) + layer_val * L1_PER_LRS + l1_idx_val
        
        formula = (
            f"ID = ({colorize('Host+ToR+DU 总数', Colors.BLUE)}) + "
            f"{colorize('rack', Colors.YELLOW)} × ({colorize('层数', Colors.BLUE)}×{colorize('每层 L1 数', Colors.BLUE)}) + "
            f"{colorize('layer', Colors.YELLOW)} × {colorize('每层 L1 数', Colors.BLUE)} + {colorize('l1_idx', Colors.YELLOW)}\n"
            f"   = ({HOST_COUNT}+{TOR_COUNT}+{DU_COUNT}) + "
            f"{colorize(str(rack_val), Colors.YELLOW)} × ({LRS_LAYERS}×{L1_PER_LRS}) + "
            f"{colorize(str(layer_val), Colors.YELLOW)} × {L1_PER_LRS} + {colorize(str(l1_idx_val), Colors.YELLOW)}\n"
            f"   = {colorize(str(HOST_COUNT+TOR_COUNT+DU_COUNT), Colors.BLUE)} + "
            f"{colorize(str(rack_val), Colors.BLUE)} × {LRS_LAYERS*L1_PER_LRS} + "
            f"{colorize(str(layer_val*L1_PER_LRS + l1_idx_val), Colors.BLUE)}\n"
            f"   = {colorize(str(HOST_COUNT+TOR_COUNT+DU_COUNT), Colors.BLUE)} + {colorize(str(offset), Colors.BLUE)}\n"
            f"   = {colorize(str(base_id + offset), Colors.BLUE)}"
        )
        return formula
    
    elif node_type == 'HRS':
        base_id = HOST_COUNT + TOR_COUNT + DU_COUNT + L1_COUNT
        hrs_id = position['hrs_id']
        
        formula = (
            f"ID = ({colorize('Host+ToR+DU+L1 总数', Colors.BLUE)}) + "
            f"{colorize('HRS 编号', Colors.YELLOW)}\n"
            f"   = ({HOST_COUNT}+{TOR_COUNT}+{DU_COUNT}+{L1_COUNT}) + "
            f"{colorize(str(hrs_id), Colors.YELLOW)}\n"
            f"   = {colorize(str(HOST_COUNT+TOR_COUNT+DU_COUNT+L1_COUNT), Colors.BLUE)} + "
            f"{colorize(str(hrs_id), Colors.BLUE)}\n"
            f"   = {colorize(str(base_id + hrs_id), Colors.BLUE)}"
        )
        return formula
    
    return "未知节点类型，无法计算公式"

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


def colorize(text, color):
    """
    给文本添加颜色
    
    参数:
        text: 要着色的文本
        color: 颜色代码 (使用 Colors 类中的常量)
    
    返回:
        带颜色的文本字符串
    """
    return f"{color}{text}{Colors.RESET}"


# ... existing code ...


def display_connection_with_formula(current_node_id, conn, port_num):
    """
    显示单条连接信息，并附带 ID 计算公式
    
    参数:
        current_node_id: 当前查询的节点 ID
        conn: 连接信息字典
        port_num: 端口编号
    """
    n_id = conn['neighbor_id']
    n_type = get_node_type(n_id)
    n_pos = get_node_position(n_id)
    
    # 端口行使用红色
    port_line = f"  [端口 {port_num}] {n_type} ID: {n_id}, 位于{format_position(n_type, n_pos)}"
    print(colorize(port_line, Colors.RED))
    
    # 带宽和延迟行保持白色
    print(f"         带宽：{conn['bandwidth']}, 延迟：{conn['delay']}")
    
    # 显示 ID 计算公式（缩进显示，使用青色以区分）
    formula_lines = explain_id_calculation(n_type, n_pos).split('\n')
    for line in formula_lines:
        print(colorize(f"         {line}", Colors.CYAN))



def display_node_info(node_id, adjacency_list):
    """显示节点详细信息"""
    node_type = get_node_type(node_id)
    position = get_node_position(node_id)
    
    print("\n" + "="*60)
    print(f"节点 ID: {node_id}")
    print(f"类型：{node_type}")
    print(f"位于{format_position(node_type, position)}")
    
    # 显示当前节点的 ID 计算公式
    print("\nID 计算公式:")
    formula_lines = explain_id_calculation(node_type, position).split('\n')
    for line in formula_lines:
        print(f"  {line}")
    
    print("="*60)
    
    if node_id not in adjacency_list:
        print("⚠️  该节点没有任何连接")
        return
    
    connections = adjacency_list[node_id]
    groups = classify_connections(node_id, connections)
    
    print("\n连接详情:")
    print("─"*60)
    
    total_ports = 0
    
    # 显示下行链路
    if groups['downlink']:
        group_name_map = {
            'Host': '下行链路 (到 Host)',
            'ToR': '下行链路 (到 ToR)',
            'DU': '下行链路 (到 DU)'
        }
        neighbor_type = get_node_type(groups['downlink'][0]['neighbor_id'])
        group_name = group_name_map.get(neighbor_type, '下行链路')
        
        print(f"\n{group_name} ({len(groups['downlink'])}个连接):")
        for idx, conn in enumerate(groups['downlink'], 1):
            display_connection_with_formula(node_id, conn, idx)
        total_ports += len(groups['downlink'])
    
    # 显示 Mesh 链路
    if groups['mesh']:
        print(f"\n同行 Mesh 链路 ({len(groups['mesh'])}个连接):")
        for idx, conn in enumerate(groups['mesh'], 1):
            display_connection_with_formula(node_id, conn, total_ports + idx)
        total_ports += len(groups['mesh'])
    
    # 显示 L1 连接
    if groups['l1_connection']:
        print(f"\nL1 连接 ({len(groups['l1_connection'])}个连接):")
        for idx, conn in enumerate(groups['l1_connection'], 1):
            display_connection_with_formula(node_id, conn, total_ports + idx)
        total_ports += len(groups['l1_connection'])
    
    # 显示上行链路
    if groups['uplink']:
        group_name_map = {
            'ToR': '上行链路 (到 ToR)',
            'DU': '上行链路 (到 DU)',
            'HRS': '上行链路 (到 HRS)'
        }
        neighbor_type = get_node_type(groups['uplink'][0]['neighbor_id'])
        group_name = group_name_map.get(neighbor_type, '上行链路')
        
        print(f"\n{group_name} ({len(groups['uplink'])}个连接):")
        for idx, conn in enumerate(groups['uplink'], 1):
            display_connection_with_formula(node_id, conn, total_ports + idx)
        total_ports += len(groups['uplink'])
    
    print("\n" + "─"*60)
    print(f"总端口数：{total_ports}")
    print("="*60)


# ... existing code ...

def main():
    """主函数"""
    print("\n" + "="*60)
    print("拓扑结构查询工具")
    print("="*60)
    print(f"数据源：{TOPOLOGY_FILE}")
    print("="*60 + "\n")
    
    # 加载拓扑
    adjacency_list = load_topology(TOPOLOGY_FILE)
    
    if adjacency_list is None:
        return
    
    print(f"✅ 成功加载拓扑，共 {len(adjacency_list)} 个有连接的节点\n")
    
    # 显示 ID 范围参考
    print("节点 ID 范围参考:")
    for node_type, (start, end) in ID_RANGES.items():
        count = end - start + 1
        print(f"  {node_type}: {start} ~ {end} (共{count}个)")
    print("\n" + "="*60 + "\n")
    
    # 交互查询循环
    while True:
        user_input = input("请输入节点 ID 进行查询（输入 q 退出）: ").strip()
        
        if user_input.lower() == 'q':
            print("👋 感谢使用，再见！")
            break
        
        try:
            node_id = int(user_input)
            
            # 检查 ID 范围
            node_type = get_node_type(node_id)
            if node_type is None:
                min_id = ID_RANGES['Host'][0]
                max_id = ID_RANGES['HRS'][1]
                print(f"❌ 错误：节点 ID {node_id} 超出有效范围 ({min_id}-{max_id})\n")
                continue
            
            # 显示节点信息
            display_node_info(node_id, adjacency_list)
            print()
            
        except ValueError:
            print("❌ 错误：请输入有效的数字或 'q' 退出\n")
        except KeyboardInterrupt:
            print("\n\n👋 程序中断，再见！")
            break
        except Exception as e:
            print(f"❌ 发生未知错误：{e}\n")


if __name__ == "__main__":
    main()