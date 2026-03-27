#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MoE Traffic Generator for ns-3-ub
生成 dispatch 和 combine 两个阶段的流量配置
"""

import numpy as np
import random
from collections import defaultdict
from typing import List, Tuple, Dict

# ==================== 配置参数 ====================
NUM_NODES = 256           # 节点总数
TOKENS_PER_NODE = 128     # 每个节点的 token 数
TOP_K = 9                 # 每个 token 选择的专家数量
DISPATCH_SIZE = 7 * 1024  # Dispatch 数据量 (7KB)
COMBINE_SIZE = 14 * 1024  # Combine 数据量 (14KB)
PRIORITY = 7              # 任务优先级
PACKET_SIZE = 4096        # 每个数据包大小 (4KB)
# # ==================== 解析概率数据 ====================
# def parse_probability_data(data_str: str) -> np.ndarray:
#     """
#     解析概率字符串并转换为行优先顺序
    
#     Args:
#         data_str: 包含 256 个数字的字符串
        
#     Returns:
#         归一化后的概率数组 (行优先顺序)
#     """
#     # 解析为数组
#     data = np.array([float(x) for x in data_str.strip().split()])
    
#     if len(data) != 256:
#         raise ValueError(f"需要 256 个概率值，实际得到 {len(data)} 个")
    
#     # 将 256 个值 reshape 为 4 个 8x8 矩阵（列优先）
#     # 原始数据是按列优先排列的：先第一列的 8 个，再第二列的 8 个...
#     matrices_col_major = []
#     idx = 0
#     for m in range(4):
#         matrix = np.zeros((8, 8))
#         for col in range(8):
#             for row in range(8):
#                 matrix[row, col] = data[idx]
#                 idx += 1
#         matrices_col_major.append(matrix)
    
#     # 转换为行优先顺序
#     # 节点编号：第 0 行是 0-7, 第 1 行是 8-15, ...
#     probability_row_major = []
#     for m in range(4):
#         matrix = matrices_col_major[m]
#         for row in range(8):
#             for col in range(8):
#                 probability_row_major.append(matrix[row, col])
    
#     probability = np.array(probability_row_major)
    
#     # 归一化
#     probability = probability / probability.sum()
    
#     return probability
def parse_probability_data(data_str: str) -> np.ndarray:
    # 解析为数组
    data = np.array([float(x) for x in data_str.strip().split()])
    
    if len(data) != 256:
        raise ValueError(f"需要 256 个概率值，实际得到 {len(data)} 个")
    
    # 直接使用原始数据（列优先顺序）
    # 因为节点编号也是列优先，所以无需转置
    probability = data.copy()
    
    # 归一化
    probability = probability / probability.sum()
    
    return probability

# ==================== 选择专家节点 ====================
def select_expert_nodes(src_node: int, probability: np.ndarray, 
                        top_k: int = TOP_K) -> List[int]:
    """
    根据概率分布为源节点选择 top_k 个专家节点
    
    Args:
        src_node: 源节点 ID
        probability: 归一化后的概率数组
        top_k: 选择的专家数量
        
    Returns:
        选中的专家节点列表
    """
    # 创建候选节点列表（排除自己）
    candidate_nodes = [i for i in range(NUM_NODES) if i != src_node]
    
    # 获取候选节点的概率权重
    candidate_probs = probability[candidate_nodes]
    
    # 归一化候选节点的概率
    candidate_probs = candidate_probs / candidate_probs.sum()
    
    # 按概率随机选择 top_k 个节点
    selected_nodes = random.choices(
        candidate_nodes, 
        weights=candidate_probs, 
        k=top_k
    )
    
    return selected_nodes

# ==================== 生成流量记录 ====================
def generate_traffic_records(probability: np.ndarray) -> List[Dict]:
    """
    生成所有节点的 dispatch 和 combine 流量记录
    
    Args:
        probability: 归一化后的概率数组
        
    Returns:
        流量记录列表，每条记录包含 taskId, sourceNode, destNode 等信息
    """
    # 用于合并相同源目的节点的流量
    # key: (src, dst), value: {'dispatch_count': count, 'combine_count': count}
    flow_stats = defaultdict(lambda: {'dispatch_count': 0, 'combine_count': 0})
    count =defaultdict(lambda:0)
    print("正在生成流量记录...")
    print(f"总节点数：{NUM_NODES}")
    print(f"每个节点 token 数：{TOKENS_PER_NODE}")
    print(f"每个 token 选择专家数：{TOP_K}")
    print(f"预计总流量记录数：约 {NUM_NODES * TOKENS_PER_NODE * TOP_K * 2} 条（合并前）")
    
    # 遍历每个节点
    for src_node in range(NUM_NODES):
        if src_node % 32 == 0:
            print(f"处理节点 {src_node}/{NUM_NODES}...")
        
        # 为每个 token 选择专家
        for token_idx in range(TOKENS_PER_NODE):
            # 选择 expert 节点
            expert_nodes = select_expert_nodes(src_node, probability, TOP_K)
            
            # 统计流量
            for dst_node in expert_nodes:
                flow_stats[(src_node, dst_node)]['dispatch_count'] += 1
                count[dst_node] += 1
                flow_stats[(src_node,dst_node )]['combine_count'] += 1
    # 按节点 ID 顺序打印，避免 defaultdict 按插入顺序导致的错位
    print("\n每个节点被选中次数（按节点 ID 0..255 顺序）:")
    for node_id in range(NUM_NODES):
        print(f"{count[node_id]} ", end='')
    print()

    # 分组统计：用于快速对比“实际被选中占比”与“输入概率质量”
    total_picks = NUM_NODES * TOKENS_PER_NODE * TOP_K
    print("\n每 64 节点分组统计（actual_share vs prob_mass）:")
    for group_id in range(NUM_NODES // 64):
        start = group_id * 64
        end = start + 64
        group_count = sum(count[node_id] for node_id in range(start, end))
        actual_share = group_count / total_picks
        prob_mass = probability[start:end].sum()
        print(
            f"  group {group_id} ({start:3d}-{end - 1:3d}): "
            f"actual={actual_share:.6f}, prob={prob_mass:.6f}, "
            f"count={group_count}"
        )
    # 生成 traffic.csv 记录
    records = []
    task_id = 0
    
    print("\n生成 Dispatch 阶段记录 (Phase 0)...")
    # Phase 0: Dispatch 阶段
    # phaseId = dst (目标节点 ID),用于 Combine 阶段依赖该节点的所有 Dispatch
    for (src, dst), stats in flow_stats.items():
        if stats['dispatch_count'] > 0:
            total_size = stats['dispatch_count'] * DISPATCH_SIZE
            records.append({
                'taskId': task_id,
                'sourceNode': src,
                'destNode': dst,
                'dataSize': total_size,
                'opType': 'URMA_WRITE',
                'priority': PRIORITY,
                'delay': '0ns',
                'phaseId': dst,    # 使用 dst 作为 phaseId，便于 Combine 依赖
                'dependOnPhases': ''
            })
            task_id += 1
    
    print(f"Dispatch 阶段记录数：{len(records)}")
    
    print("\n生成 Combine 阶段记录 (Phase 1, 依赖 Phase 0)...")
    # Phase 1: Combine 阶段
    # dependOnPhases = dst，表示依赖所有发往节点 dst 的 Dispatch 任务
    combine_count = 0
    # for (src, dst), stats in flow_stats.items():
    #     if stats['combine_count'] > 0:
    #         total_size = stats['combine_count'] * COMBINE_SIZE
    #         records.append({
    #             'taskId': task_id,
    #             'sourceNode': dst, # Combine: dst → src (返回结果)
    #             'destNode': src,
    #             'dataSize': total_size,
    #             'opType': 'URMA_WRITE',
    #             'priority': PRIORITY,
    #             'delay': '100ns',
    #             'phaseId': dst+1000,  # 避免与 Dispatch 的 phaseId 重复（可选）
    #             'dependOnPhases': dst   # 依赖所有 phaseId=dst 的 Dispatch 任务
    #         })
    #         task_id += 1
    #         combine_count += 1
    
    print(f"Combine 阶段记录数：{combine_count}")
    print(f"总记录数：{len(records)}")

     # 计算数据包数量统计
    dispatch_total_packets = 0
    combine_total_packets = 0
    
    for record in records:
        data_size = record['dataSize']
        # 向上取整计算需要的数据包数量
        packet_count = (data_size + PACKET_SIZE - 1) // PACKET_SIZE
        if record['phaseId'] < 1000:  # Dispatch 阶段
            dispatch_total_packets += packet_count
        else:  # Combine 阶段
            combine_total_packets += packet_count
    
    print(f"\n数据包统计信息（每个数据包 {PACKET_SIZE}B）:")
    print(f"  Dispatch 阶段总数据包数：{dispatch_total_packets:,}")
    print(f"  Combine 阶段总数据包数：{combine_total_packets:,}")
    print(f"  总数据包数：{dispatch_total_packets + combine_total_packets:,}")

    
    return records

# ==================== 写入 CSV 文件 ====================
def write_traffic_csv(records: List[Dict], output_file: str):
    """
    将流量记录写入 CSV 文件
    
    Args:
        records: 流量记录列表
        output_file: 输出文件路径
    """
    with open(output_file, 'w', encoding='utf-8') as f:
        # 写入表头
        f.write("taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n")
        
        # 写入记录
        for record in records:
            line = f"{record['taskId']},{record['sourceNode']},{record['destNode']}," \
                   f"{record['dataSize']},{record['opType']},{record['priority']}," \
                   f"{record['delay']},{record['phaseId']},{record['dependOnPhases']}\n"
            f.write(line)
    
    print(f"\n✓ 流量配置已保存到：{output_file}")

# ==================== 主函数 ====================
def main():
    # 设置随机种子以保证可重复性
    random.seed(42)
    np.random.seed(42)
    
    # 原始概率数据
    prob_str2 = """926       1089         1062         7359         3292         1886         2795         598  3846         952  984  4647         4615         1731         3918         2437         776  274  1007         1543         5647         2066         1084         5613         503  1903         718  961  2762         1112         665  774  1283         1596         1523         706  394  1815         651  2633         7895         982  1432         3591         2181         4354         2348         808  639  1757         2673         828  915  154  1476         4159         703  438  536  801  1498         6995         690  474  2796         514  781  1075         580  589  578  1789         1939         607  811  275  339  1091         5617         3109         794  1148         1137         1595         2585         1086         438  1309         1753         869  5300         788  983  1725         779  1047         883  421  2373         812  396  1350         659  1611         633  837  1174         356  1558         1074         994         1082         2165         382  1650         338  720  934  542  5192         968  1006         1919         2220         1997         721  1707         920  710  2551         718  2177         189  1058         2679         1179         1717         3091         862  873  1717         1505         2975         710  2516         4113         435  1519         1455         741  660  360  822  753  3762         2289         1268         1374         922  498  707  1227         605  868  957  1068         125  2327         1288         1622         368  3972         2287         1061         1916         537  421  1016         1524         1605         569  3921         1441         680  722  888  581  3015         1899         822         1117         1120         146  3354         3087         1461         737  1257         1875         805  495  5661         386  1119         2814         485  776  3153         2049         1173         874  5752         788  863  965  2246         2871         615  212  1359         669  619         4538         1809         285  600  2434         1834         173  498  597  3644         671  473  2370         4852         2066         1560         6669         602  1421         978  2033         2181         2400         1254         1264         1381         607  813  2262         788  350         577  1223         1360
    """
    
    print("=" * 60)
    print("MoE Traffic Generator for ns-3-ub")
    print("=" * 60)
    
    # Step 1: 解析概率数据
    print("\n[Step 1] 解析并转换概率数据...")
    probability = parse_probability_data(prob_str2)
    print(f"✓ 概率数组形状：{probability.shape}")
    print(f"✓ 概率和：{probability.sum():.6f}")
    print(f"✓ 最大概率：{probability.max():.6f} (节点 {np.argmax(probability)})")
    print(f"✓ 最小概率：{probability.min():.6f}")
    
    # Step 2: 生成流量记录
    print("\n[Step 2] 生成流量记录...")
    records = generate_traffic_records(probability)
    
    # Step 3: 写入 CSV 文件
    print("\n[Step 3] 写入 CSV 文件...")
    output_file = "/app/ns-3-ub/scratch/tp_UB_Mesh_V0001/traffic.csv"
    write_traffic_csv(records, output_file)
    
    # Step 4: 统计信息
    print("\n" + "=" * 60)
    print("流量统计信息")
    print("=" * 60)
    
    dispatch_records = [r for r in records if r['phaseId'] <1000]
    combine_records = [r for r in records if r['phaseId'] >=1000]
    
    dispatch_total_data = sum(r['dataSize'] for r in dispatch_records)
    combine_total_data = sum(r['dataSize'] for r in combine_records)

     # 计算数据包数量
    dispatch_total_packets = sum((r['dataSize'] + PACKET_SIZE - 1) // PACKET_SIZE for r in dispatch_records)
    combine_total_packets = sum((r['dataSize'] + PACKET_SIZE - 1) // PACKET_SIZE for r in combine_records)
    
    
    print(f"Dispatch 阶段记录数：{len(dispatch_records)}")
    print(f"Combine 阶段记录数：{len(combine_records)}")
    print(f"总记录数：{len(records)}")
    print(f"Dispatch 总数据量：{dispatch_total_data / 1024 / 1024:.2f} MB")
    print(f"Combine 总数据量：{combine_total_data / 1024 / 1024:.2f} MB")
    print(f"总数据量：{(dispatch_total_data + combine_total_data) / 1024 / 1024:.2f} MB")
    
    # 验证流量守恒
    print(f"\n流量守恒验证:")
    print(f"  Dispatch 平均每条流：{dispatch_total_data / len(dispatch_records):.0f} bytes")
    #print(f"  Combine 平均每条流：{combine_total_data / len(combine_records):.0f} bytes")
    print(f"  比例：{combine_total_data / dispatch_total_data:.2f} (应为 2.0)")
    
    print(f"\n数据包详细统计（每个数据包 {PACKET_SIZE}B = 4KB）:")
    print(f"  Dispatch 阶段:")
    print(f"    - 总数据包数：{dispatch_total_packets:,} 个")
    print(f"    - 平均每流数据包数：{dispatch_total_packets / len(dispatch_records):.2f} 个")
    print(f"  Combine 阶段:")
    print(f"    - 总数据包数：{combine_total_packets:,} 个")
    #print(f"    - 平均每流数据包数：{combine_total_packets / len(combine_records):.2f} 个")
    print(f"  合计:")
    print(f"    - 总数据包数：{dispatch_total_packets + combine_total_packets:,} 个")
    
    print("\n✓ 生成完成！")

if __name__ == "__main__":
    main()