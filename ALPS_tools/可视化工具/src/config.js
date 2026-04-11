export const NODE_COLORS = {
  Host: 0x145da0,
  ToR: 0xf18f01,
  DU: 0x3ca370,
  L1: 0x7f5539,
  HRS: 0xbc4749,
};

export const NODE_SIZE = {
  Host: 0.24,
  ToR: 0.24,
  DU: 0.28,
  L1: 0.32,
  HRS: 0.38,
};

export const LAYOUT_FILE = "./topology_layout.json";

// 可视化工具目录 -> scratch/tp_UB_Mesh_V0002/self_run_log/port_metrics_1us.csv
export const PORT_METRICS_FILES = [
  "./port_metrics_1us.csv",
  "../../../tp_UB_Mesh_V0002/self_run_log/port_metrics_1us.csv",
  "/scratch/tp_UB_Mesh_V0002/self_run_log/port_metrics_1us.csv",
];

export const QUEUE_LIMIT_BYTES = 1024 * 1024;
export const UTILIZATION_MAX = 1.0;

export const PLAYBACK_DEFAULT_SPEED = 1.0;
export const PLAYBACK_FRAME_INTERVAL_MS = 80;
export const EDGE_BASE_RADIUS = 0.015;
export const EDGE_RADIUS_GAIN = 0.05;
