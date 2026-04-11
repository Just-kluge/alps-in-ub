import {
  LAYOUT_FILE,
  PATH_SEARCH_FILES,
  PIT_INDEX_FILES,
  PORT_METRICS_FILES,
} from "./config.js";

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

function makePathKey(sourceNode, destNode) {
  return `${sourceNode}-${destNode}`;
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

export async function loadPitPathIndex() {
  const { response, sourcePath } = await fetchByCandidates(PIT_INDEX_FILES);
  const text = await response.text();
  const lines = text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter(Boolean);

  if (lines.length < 2) {
    throw new Error("PIT 文件为空或缺少数据行");
  }

  const header = parseCsvLine(lines[0]);
  const idxPathId = header.indexOf("path_id");
  const idxTraverseNodes = header.indexOf("traverse_nodes");
  if (idxPathId < 0 || idxTraverseNodes < 0) {
    throw new Error("PIT 表头缺少 path_id 或 traverse_nodes 字段");
  }

  const pathMap = new Map();
  for (let i = 1; i < lines.length; i += 1) {
    const cols = parseCsvLine(lines[i]);
    if (cols.length <= Math.max(idxPathId, idxTraverseNodes)) {
      continue;
    }

    const pathId = toNumber(cols[idxPathId], -1);
    if (pathId < 0) {
      continue;
    }

    const nodes = cols[idxTraverseNodes]
      .split(/\s+/)
      .map((v) => Number(v))
      .filter((v) => Number.isFinite(v));

    if (nodes.length < 2) {
      continue;
    }

    pathMap.set(pathId, {
      pathId,
      sourceNode: nodes[0],
      destNode: nodes[nodes.length - 1],
      nodes,
    });
  }

  return { sourcePath, pathMap };
}

export async function loadPathSearchIndex() {
  const { response, sourcePath } = await fetchByCandidates(PATH_SEARCH_FILES);
  const text = await response.text();
  const lines = text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter(Boolean);

  if (lines.length < 2) {
    throw new Error("path_search 文件为空或缺少数据行");
  }

  const header = parseCsvLine(lines[0]);
  const idxSourceNode = header.indexOf("sourceNode");
  const idxDestNode = header.indexOf("destNode");
  const idxPathIds = header.indexOf("path_ids");
  if (idxSourceNode < 0 || idxDestNode < 0 || idxPathIds < 0) {
    throw new Error("path_search 表头缺少 sourceNode / destNode / path_ids 字段");
  }

  const pathMap = new Map();
  for (let i = 1; i < lines.length; i += 1) {
    const cols = parseCsvLine(lines[i]);
    if (cols.length <= Math.max(idxSourceNode, idxDestNode, idxPathIds)) {
      continue;
    }

    const sourceNode = toNumber(cols[idxSourceNode], -1);
    const destNode = toNumber(cols[idxDestNode], -1);
    if (sourceNode < 0 || destNode < 0) {
      continue;
    }

    const pathIds = cols[idxPathIds]
      .split(/\s+/)
      .map((v) => Number(v))
      .filter((v) => Number.isFinite(v));

    if (!pathIds.length) {
      continue;
    }

    pathMap.set(makePathKey(sourceNode, destNode), {
      sourceNode,
      destNode,
      pathIds,
      numPaths: toNumber(cols[header.indexOf("num_paths")], pathIds.length),
    });
  }

  return { sourcePath, pathMap };
}
