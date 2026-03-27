UB Mesh 新拓扑 PIT 生成规则（精简可执行版）

一、目标与硬约束
1. 本文用于定义 PIT 路径枚举规则，供 build_ub_mesh_pst_pit.py 实现。
2. 路径按“端口区分”写入 PIT：
同一节点序列，只要 output_ports 不同，就视为不同路径。
3. 场景一（同 Rack 同列）严格只输出 7 条模板路径，不允许补充更长可行路径。
4. 操作一（Layer 0/1 且偶数 DU 的 +/-4 映射）是硬规则，必须写死。
5. 场景二中 L1 目标 DU 奇偶是固定约束：
L1[0]/L1[1] 只能到偶数 DU（DU[2j]），L1[2]/L1[3] 只能到奇数 DU（DU[2j+1]）。
6. 场景三中源 DU->HRS 不参与路径分叉计数，每条路径只取 1 条 DU->HRS 链路。

二、拓扑事实（与 user_topo_pod_lrs_hrs_256NPU.py 一致）
1. 节点规模
Host: 256（1 pod, 4 rack, 每 rack 8x8）
ToR: 256（与 Host 一一对应）
DU: 256（每 rack 4 layer，每层 16）
L1: 64（每 rack 4 layer，每层 4）
HRS: 4（HRS[i] 服务 layer i）

2. 关键连线
ToR-ToR: 仅同列 full mesh（无同行直连）
ToR-DU: 第 i 列 ToR 连每层 DU[2i] 与 DU[2i+1]
DU-L1: 偶数 DU 连 L1[0]/L1[1]；奇数 DU 连 L1[2]/L1[3]；每条 DU-L1 为 2 物理链路
DU-HRS: layer i 的 DU 连 HRS[i]；每条 DU-HRS 为 4 物理链路
DU-ToR: DU 可下行到本列全部 8 个 ToR

三、三类通信场景

3.1 场景一：同 Rack 且同列（row 不同）
1. 路径模板
Host(src) -> ToR(src) -> [可选 ToR(mid)] -> ToR(dst) -> Host(dst)
2. 枚举规则（严格）
直连 1 条: ToR(src) -> ToR(dst)
单中转 6 条: mid 取同列除 src/dst 外 6 个 ToR
3. 总数
固定 7 条，不允许额外路径。

3.2 场景二：同 Rack 跨列
1. 路径模板
Host(src) -> ToR(src) -> DU_src -> L1_src -> DU_dst -> [可选 ToR(mid)] -> ToR(dst) -> Host(dst)

2. 步骤 1：源端上行（固定，无分叉）
设源列为 i，按 src_row 固定选择 DU/L1/链路序号：

| src_row | 源 DU | 源 L1 | DU->L1 物理链路 |
|---|---|---|---|
| 0 | DU[2i] | L1[0] | 第 1 条 |
| 1 | DU[2i] | L1[0] | 第 2 条 |
| 2 | DU[2i] | L1[1] | 第 1 条 |
| 3 | DU[2i] | L1[1] | 第 2 条 |
| 4 | DU[2i+1] | L1[2] | 第 1 条 |
| 5 | DU[2i+1] | L1[2] | 第 2 条 |
| 6 | DU[2i+1] | L1[3] | 第 1 条 |
| 7 | DU[2i+1] | L1[3] | 第 2 条 |

3. 步骤 2：L1 到目的 DU（分叉）
设目的列为 j，目标 DU 由源 L1 唯一决定：
L1[0]/L1[1] -> DU[2j]（偶数 DU），
L1[2]/L1[3] -> DU[2j+1]（奇数 DU）。
在该唯一目标 DU 上，按 2 条物理链路形成 2 条候选路径。

4. 步骤 3：layer 枚举
layer 取 0..3，共 4 层。

5. 步骤 4：目的 DU 应用操作一
| layer | 目的 DU 奇偶 | DU->ToR 输出规则 | 是否中转 |
|---|---|---|---|
| 2,3 | 任意 | 直接到 ToR(dst_row, dst_col) | 否 |
| 0 | 偶数 | 只能先到前 4 行 ToR(0..3, dst_col) | 若 dst_row>=4，先到 row=dst_row-4，再同列中转 +4 |
| 0 | 奇数 | 直接到 ToR(dst_row, dst_col) | 否 |
| 1 | 偶数 | 只能先到后 4 行 ToR(4..7, dst_col) | 若 dst_row<4，先到 row=dst_row+4，再同列中转 -4 |
| 1 | 奇数 | 直接到 ToR(dst_row, dst_col) | 否 |

6. 总数
4 层 x 2 条 L1->DU 端口分叉 = 8 条。

3.3 场景三：同 Pod 跨 Rack
1. 路径模板
Host(src) -> ToR(src) -> DU_src -> HRS[k] -> DU_dst -> [可选 ToR(mid)] -> ToR(dst) -> Host(dst)

2. 步骤 1+2：源端上行（固定，无分叉，和场景二相反）
设源列为 i，按 src_row 固定选择源 DU，然后连接到当前 layer 对应的 HRS[k]（k=0..3）。
DU->HRS 虽有 4 条物理链路，但不参与路径分叉计数，每条路径仅使用 1 条。

| src_row | 源 DU | 目标 HRS | DU->HRS 使用方式 |
|---|---|---|---|
| 0 | DU[2i+1] | HRS[k] | 4 条物理链路中固定取 第1 条 |
| 1 | DU[2i+1] | HRS[k] | 4 条物理链路中固定取 第2 条 |
| 2 | DU[2i+1] | HRS[k] | 4 条物理链路中固定取 第3 条 |
| 3 | DU[2i+1] | HRS[k] | 4 条物理链路中固定取 第4 条 |
| 4 | DU[2i] | HRS[k] | 4 条物理链路中固定取 第1 条 |
| 5 | DU[2i] | HRS[k] | 4 条物理链路中固定取 第2 条 |
| 6 | DU[2i] | HRS[k] | 4 条物理链路中固定取 第3 条 |
| 7 | DU[2i] | HRS[k] | 4 条物理链路中固定取 第4 条 |

4. 步骤 3：HRS[k] 到目的 DU（分叉）
设目的列为 j：
到 DU[2j] 有 4 条物理链路，
到 DU[2j+1] 有 4 条物理链路。
合计 8 条候选路径（端口区分）。

5. 步骤 4：layer 枚举
k 取 0..3，共 4 层。

6. 步骤 5：目的 DU 应用操作一
与场景二完全相同。

7. 总数
4 层 x 8 条 HRS->DU 端口分叉 = 32 条。

四、实现注意事项
1. 必须基于 topology.csv 的真实端口连接写 output_ports。
2. 端口不同即路径不同，节点序列相同也要保留多条。
3. 场景分类优先级：
先判同 rack 同列，再判同 rack 跨列，再判同 pod 跨 rack。
4. 若某模板路径在 topology.csv 不存在，不可伪造：跳过该路径并继续生成，同时记录差异日志。

五、快速自检
1. 任取同 rack 同列主机对，正向目标上限 7 条；若缺边则少于 7 且应有日志。
2. 任取同 rack 跨列主机对，正向目标上限 8 条；若缺边则少于 8 且应有日志。
3. 任取同 pod 跨 rack 主机对，正向目标上限 32 条；若缺边则少于 32 且应有日志。
4. 反向路径应一一可构造并写 reverse_path_id。
