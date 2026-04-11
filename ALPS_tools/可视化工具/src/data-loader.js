import { LAYOUT_FILE, PORT_METRICS_FILES } from "./config.js";

export async function loadTopologyLayout() {
  const response = await fetch(LAYOUT_FILE);
  if (!response.ok) {
    throw new Error(`无法读取布局文件: ${LAYOUT_FILE}`);
  }
  return response.json();
}

function parseCsvLine(line) {
  return line.split(",").map((v) => v.trim());
}

function toNumber(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function makePortKey(nodeId, portId) {
  return `${nodeId}:${portId}`;
}

async function fetchByCandidates(paths) {
  let lastError = null;
  for (const path of paths) {
    try {
      const response = await fetch(path);
      if (response.ok) {
        return { response, sourcePath: path };
      }
      lastError = new Error(`HTTP ${response.status} at ${path}`);
    } catch (err) {
      lastError = err;
    }
  }
  throw new Error(`无法读取端口指标文件，尝试路径失败: ${paths.join(" | ")}\n${lastError || ""}`);
}

export async function loadPortMetricsTimeline() {
  const { response, sourcePath } = await fetchByCandidates(PORT_METRICS_FILES);

  const text = await response.text();
  const lines = text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter(Boolean);

  if (lines.length < 2) {
    throw new Error("端口指标文件为空或缺少数据行");
  }

  const header = parseCsvLine(lines[0]);
  const colIndex = {
    timestamp_us: header.indexOf("timestamp_us"),
    window_us: header.indexOf("window_us"),
    nodeId: header.indexOf("nodeId"),
    portId: header.indexOf("portId"),
    portCapacityGbps: header.indexOf("portCapacityGbps"),
    txBytes: header.indexOf("txBytes"),
    bandwidthUtilization: header.indexOf("bandwidthUtilization"),
    queueBytes: header.indexOf("queueBytes"),
  };

  if (Object.values(colIndex).some((i) => i < 0)) {
    throw new Error("端口指标CSV字段不完整，请检查表头");
  }

  const frameMap = new Map();
  let windowUs = 1;

  for (let i = 1; i < lines.length; i += 1) {
    const cols = parseCsvLine(lines[i]);
    if (cols.length < header.length) {
      continue;
    }

    const timestampUs = toNumber(cols[colIndex.timestamp_us], -1);
    if (timestampUs < 0) {
      continue;
    }

    const currentWindowUs = toNumber(cols[colIndex.window_us], 1);
    if (currentWindowUs > 0) {
      windowUs = currentWindowUs;
    }

    const nodeId = toNumber(cols[colIndex.nodeId], -1);
    const portId = toNumber(cols[colIndex.portId], -1);
    if (nodeId < 0 || portId < 0) {
      continue;
    }

    if (!frameMap.has(timestampUs)) {
      frameMap.set(timestampUs, new Map());
    }

    frameMap.get(timestampUs).set(makePortKey(nodeId, portId), {
      timestampUs,
      windowUs: currentWindowUs,
      nodeId,
      portId,
      portCapacityGbps: toNumber(cols[colIndex.portCapacityGbps], 0),
      txBytes: toNumber(cols[colIndex.txBytes], 0),
      bandwidthUtilization: toNumber(cols[colIndex.bandwidthUtilization], 0),
      queueBytes: toNumber(cols[colIndex.queueBytes], 0),
    });
  }

  const timestamps = Array.from(frameMap.keys()).sort((a, b) => a - b);
  if (!timestamps.length) {
    throw new Error("端口指标CSV未解析到有效时间帧");
  }

  return {
    timestamps,
    frameMap,
    windowUs,
    sourcePath,
  };
}

export function getPortMetric(frameMap, timestampUs, nodeId, portId) {
  const frame = frameMap.get(timestampUs);
  if (!frame) {
    return null;
  }
  return frame.get(makePortKey(nodeId, portId)) || null;
}
