import {
  getPortMetric,
  loadPathSearchIndex,
  loadPitPathIndex,
  loadPortMetricsTimeline,
  loadTopologyLayout,
} from "./data-loader.js";
import {
  PLAYBACK_DEFAULT_SPEED,
  PLAYBACK_FRAME_INTERVAL_MS,
  QUEUE_LIMIT_BYTES,
} from "./config.js";
import { createTopologyViewer } from "./topology-scene.js";

const NODE_LAYER_LEVEL = {
  Host: 0,
  ToR: 1,
  DU: 2,
  L1: 3,
  HRS: 4,
};

function classifyPortDirection(localType, peerType) {
  const local = NODE_LAYER_LEVEL[localType];
  const peer = NODE_LAYER_LEVEL[peerType];
  if (local == null || peer == null) {
    return "same";
  }
  if (peer > local) {
    return "up";
  }
  if (peer < local) {
    return "down";
  }
  return "same";
}

function buildNodePortIndex(layout) {
  const nodeTypeMap = new Map(layout.nodes.map((n) => [n.id, n.type]));
  const index = new Map();

  function ensurePort(nodeId, portId) {
    if (!index.has(nodeId)) {
      index.set(nodeId, new Map());
    }
    const portMap = index.get(nodeId);
    if (!portMap.has(portId)) {
      portMap.set(portId, {
        portId,
        category: "same",
        peers: [],
      });
    }
    return portMap.get(portId);
  }

  function addPort(nodeId, portId, peerNodeId) {
    const localType = nodeTypeMap.get(nodeId) || "Host";
    const peerType = nodeTypeMap.get(peerNodeId) || "Host";
    const category = classifyPortDirection(localType, peerType);

    const portInfo = ensurePort(nodeId, portId);
    portInfo.peers.push({ nodeId: peerNodeId, type: peerType });
    portInfo.category = category;
  }

  layout.edges.forEach((edge) => {
    addPort(edge.src, edge.srcPort, edge.dst);
    addPort(edge.dst, edge.dstPort, edge.src);
  });

  return index;
}

function formatMetric(metricMode, metric) {
  if (!metric) {
    return { score: 0, text: "0" };
  }

  if (metricMode === "utilization") {
    const util = Math.max(0, Number(metric.bandwidthUtilization) || 0);
    return {
      score: util,
      text: `${(util * 100).toFixed(1)}%`,
    };
  }

  const q = Math.max(0, Number(metric.queueBytes) || 0);
  const qPct = Math.min(100, (q / QUEUE_LIMIT_BYTES) * 100);
  return {
    score: q,
    text: `${qPct.toFixed(1)}%`,
  };
}

function renderPortList(el, items) {
  if (!el) {
    return;
  }
  if (!items.length) {
    el.textContent = "-";
    return;
  }
  el.textContent = items.map((item) => `${item.portId}: ${item.text}`).join("\n");
}

function setColumnVisible(colEl, visible) {
  if (!colEl) {
    return;
  }
  colEl.classList.toggle("is-empty", !visible);
}

async function bootstrap() {
  const statsEl = document.getElementById("stats");
  const canvas = document.getElementById("topology-canvas");
  const metricSelect = document.getElementById("metric-select");
  const playBtn = document.getElementById("play-btn");
  const pauseBtn = document.getElementById("pause-btn");
  const slider = document.getElementById("time-slider");
  const timeLabel = document.getElementById("time-label");
  const statusLabel = document.getElementById("playback-status");
  const debugLog = document.getElementById("debug-log");
  const nodeInfo = document.getElementById("node-info");
  const sidePanel = document.getElementById("side-panel");
  const panelBody = document.getElementById("panel-body");
  const panelToggleBtn = document.getElementById("panel-toggle");
  const nppSub = document.getElementById("npp-sub");
  const nppUp = document.getElementById("npp-up");
  const nppDown = document.getElementById("npp-down");
  const nppSame = document.getElementById("npp-same");
  const nppColUp = document.getElementById("npp-col-up");
  const nppColDown = document.getElementById("npp-col-down");
  const nppColSame = document.getElementById("npp-col-same");
  const nodeQueryInput = document.getElementById("node-query-input");
  const nodeQueryBtn = document.getElementById("node-query-btn");
  const nppSearchTip = document.getElementById("npp-search-tip");
  const pathSrcInput = document.getElementById("path-src-input");
  const pathDstInput = document.getElementById("path-dst-input");
  const pathFindBtn = document.getElementById("path-find-btn");
  const pathResetBtn = document.getElementById("path-reset-btn");
  const pathStatus = document.getElementById("path-status");

  const debugLines = [];
  function debug(msg) {
    const ts = new Date().toLocaleTimeString();
    const line = `[${ts}] ${msg}`;
    console.log(`[viewer-debug] ${msg}`);
    debugLines.push(line);
    while (debugLines.length > 30) {
      debugLines.shift();
    }
    if (debugLog) {
      debugLog.textContent = debugLines.join("\n");
      debugLog.scrollTop = debugLog.scrollHeight;
    }
  }

  if (debugLog) {
    debugLog.textContent = "debug: bootstrap start";
  }
  if (statusLabel) {
    statusLabel.textContent = "status: bootstrapping";
  }

  window.addEventListener("error", (e) => {
    const msg = e?.message || "unknown error";
    debug(`window.error: ${msg}`);
  });

  window.addEventListener("unhandledrejection", (e) => {
    const reason = e?.reason;
    const msg = reason?.stack || reason?.message || String(reason);
    debug(`unhandledrejection: ${msg}`);
  });

  debug("bootstrap enter");

  if (panelToggleBtn && sidePanel && panelBody) {
    panelToggleBtn.addEventListener("click", () => {
      const collapsed = sidePanel.classList.toggle("panel-collapsed");
      panelToggleBtn.textContent = collapsed ? "展开" : "收起";
      panelToggleBtn.setAttribute("aria-expanded", collapsed ? "false" : "true");
      debug(`panel ${collapsed ? "collapsed" : "expanded"}`);
    });
  }

  try {
    const layout = await loadTopologyLayout();
    debug(`layout loaded: nodes=${layout.meta.nodeCount}, edges=${layout.meta.edgeCount}`);

    const viewer = createTopologyViewer(canvas, layout);
    const nodePortIndex = buildNodePortIndex(layout);
    let selectedNodeId = null;
    let pitPathMap = new Map();
    let pathSearchMap = new Map();
    let pitReady = false;
    let pathSearchReady = false;

    async function ensurePathIndexesLoaded(forceReload = false) {
      if (!forceReload && pitReady && pathSearchReady) {
        return true;
      }

      try {
        const pit = await loadPitPathIndex();
        pitPathMap = pit.pathMap;
        pitReady = true;
        debug(`pit loaded: paths=${pitPathMap.size}, source=${pit.sourcePath}`);
      } catch (err) {
        const em = String(err?.message || err);
        debug(`pit load failed: ${em}`);
        pitReady = false;
      }

      try {
        const search = await loadPathSearchIndex();
        pathSearchMap = search.pathMap;
        pathSearchReady = true;
        debug(`path_search loaded: pairs=${pathSearchMap.size}, source=${search.sourcePath}`);
      } catch (err) {
        const em = String(err?.message || err);
        debug(`path_search load failed: ${em}`);
        pathSearchReady = false;
      }

      if (pathStatus) {
        pathStatus.textContent = pitReady && pathSearchReady
          ? "路径索引已加载，可查找候选路径"
          : "路径索引未加载，无法查找路径";
      }
      return pitReady && pathSearchReady;
    }

    viewer.setNodeSelectHandler((node) => {
      selectedNodeId = node.id;
      nodeInfo.textContent = `节点 ${node.id} | 类型 ${node.type} | 位置 (${node.x}, ${node.y}, ${node.z})`;
      if (nodeQueryInput) {
        nodeQueryInput.value = String(node.id);
      }
      renderNodePortPanel();
    });

    let timestamps = [0];
    let frameMap = new Map();
    let windowUs = 1;
    let sourcePath = "(not loaded)";
    let metricsReady = false;

    try {
      const timeline = await loadPortMetricsTimeline();
      timestamps = timeline.timestamps;
      frameMap = timeline.frameMap;
      windowUs = timeline.windowUs;
      sourcePath = timeline.sourcePath;
      metricsReady = true;
      debug(`timeline loaded: frames=${timestamps.length}, windowUs=${windowUs}, source=${sourcePath}`);
    } catch (err) {
      const em = String(err?.message || err);
      debug(`timeline load failed, continue without metrics: ${em}`);
      statsEl.textContent =
        `节点: ${layout.meta.nodeCount} | 链路: ${layout.meta.edgeCount} | 指标文件未加载，先显示静态拓扑`;
    }

    await ensurePathIndexesLoaded();

    let metricMode = metricSelect.value;
    let frameIdx = 0;
    let isPlaying = false;
    let carryMs = 0;
    let lastTs = 0;
    let lastStatusUpdateMs = 0;

    slider.min = "0";
    slider.max = String(Math.max(timestamps.length - 1, 0));
    slider.value = "0";
    debug(`slider init: min=${slider.min}, max=${slider.max}, value=${slider.value}`);

    if (!metricsReady) {
      slider.disabled = true;
      playBtn.disabled = true;
      pauseBtn.disabled = true;
      if (statusLabel) {
        statusLabel.textContent = "status: metrics-missing (topology-only)";
      }
    }

    function makeResolver(ts) {
      if (!metricsReady) {
        return () => null;
      }
      return (nodeId, portId) => getPortMetric(frameMap, ts, nodeId, portId);
    }

    function renderNodePortPanel() {
      if (!nppSub) {
        return;
      }

      if (selectedNodeId == null) {
        nppSub.textContent = "点击节点后显示端口分组";
        renderPortList(nppUp, []);
        renderPortList(nppDown, []);
        renderPortList(nppSame, []);
        setColumnVisible(nppColUp, false);
        setColumnVisible(nppColDown, false);
        setColumnVisible(nppColSame, false);
        return;
      }

      const ts = timestamps[Math.max(0, Math.min(frameIdx, timestamps.length - 1))];
      const portMap = nodePortIndex.get(selectedNodeId) || new Map();
      const grouped = {
        up: [],
        down: [],
        same: [],
      };

      Array.from(portMap.values()).forEach((portInfo) => {
        const metric = metricsReady
          ? getPortMetric(frameMap, ts, selectedNodeId, portInfo.portId)
          : null;
        const value = formatMetric(metricMode, metric);
        grouped[portInfo.category].push({
          portId: portInfo.portId,
          text: value.text,
          score: value.score,
        });
      });

      grouped.up.sort((a, b) => b.score - a.score);
      grouped.down.sort((a, b) => b.score - a.score);
      grouped.same.sort((a, b) => b.score - a.score);

      setColumnVisible(nppColUp, grouped.up.length > 0);
      setColumnVisible(nppColDown, grouped.down.length > 0);
      setColumnVisible(nppColSame, grouped.same.length > 0);

      const modeTxt = metricMode === "utilization" ? "利用率" : "队列占比";
      nppSub.textContent = `节点 ${selectedNodeId} | t=${ts}us | 排序: ${modeTxt}`;
      renderPortList(nppUp, grouped.up);
      renderPortList(nppDown, grouped.down);
      renderPortList(nppSame, grouped.same);
    }

    function renderFrame(index) {
      frameIdx = Math.max(0, Math.min(index, timestamps.length - 1));
      const ts = timestamps[frameIdx];
      const summary = viewer.setFrameMetricResolver(makeResolver(ts));

      slider.value = String(frameIdx);
      timeLabel.textContent = `t = ${ts} us | 窗口 = ${windowUs} us`;
      if (statusLabel) {
        statusLabel.textContent = `status: ${isPlaying ? "playing" : "paused"} | frame=${frameIdx}/${timestamps.length - 1}`;
      }

      const queueMB = (summary.queueMax / (1024 * 1024)).toFixed(3);
      statsEl.textContent =
        `节点: ${layout.meta.nodeCount} | 链路: ${layout.meta.edgeCount}` +
        ` | 指标: ${metricMode === "queue" ? "Queue" : "Utilization"}` +
        ` | 样本端口: ${summary.samples}` +
        ` | MaxQueue: ${queueMB} MB` +
        ` | MaxUtil: ${(summary.utilMax * 100).toFixed(1)}%` +
        ` | 数据源: ${sourcePath}`;

      if (frameIdx === 0 || frameIdx === timestamps.length - 1 || frameIdx % 20 === 0) {
        debug(`render frame=${frameIdx}, t=${ts}us, samples=${summary.samples}`);
      }

      renderNodePortPanel();
    }

    function setMetricMode(mode) {
      metricMode = mode === "utilization" ? "utilization" : "queue";
      viewer.setMetricMode(metricMode);
      renderFrame(frameIdx);
    }

    function queryNodeAndFocus() {
      if (!nodeQueryInput || !nodeQueryBtn) {
        return;
      }
      const raw = String(nodeQueryInput.value || "").trim();
      const nodeId = Number(raw);
      if (!Number.isFinite(nodeId)) {
        if (nppSearchTip) {
          nppSearchTip.textContent = "请输入有效节点ID";
        }
        debug(`node query invalid: ${raw}`);
        return;
      }

      const node = viewer.getNodeById(nodeId);
      if (!node) {
        if (nppSearchTip) {
          nppSearchTip.textContent = `未找到节点 ${nodeId}`;
        }
        debug(`node query miss: ${nodeId}`);
        return;
      }

      const focused = viewer.focusNodeById(nodeId);
      if (!focused) {
        if (nppSearchTip) {
          nppSearchTip.textContent = `节点 ${nodeId} 存在，但无法聚焦`;
        }
        debug(`node focus failed: ${nodeId}`);
        return;
      }

      selectedNodeId = nodeId;
      nodeInfo.textContent = `节点 ${node.id} | 类型 ${node.type} | 位置 (${node.x}, ${node.y}, ${node.z})`;
      if (nppSearchTip) {
        nppSearchTip.textContent = `已定位节点 ${nodeId}`;
      }
      debug(`node query focus: ${nodeId}`);
      renderNodePortPanel();
    }

    async function findPathsAndHighlight() {
      if (!(await ensurePathIndexesLoaded())) {
        if (pathStatus) {
          pathStatus.textContent = "路径索引未加载，无法查找路径";
        }
        return;
      }

      const src = Number(String(pathSrcInput?.value || "").trim());
      const dst = Number(String(pathDstInput?.value || "").trim());
      if (!Number.isFinite(src) || !Number.isFinite(dst)) {
        if (pathStatus) {
          pathStatus.textContent = "请输入有效的源/目的Host节点ID";
        }
        return;
      }

      const key = `${src}-${dst}`;
      const searchEntry = pathSearchMap.get(key);
      if (!searchEntry) {
        viewer.clearPathHighlight();
        if (pathStatus) {
          pathStatus.textContent = `未找到 ${src} -> ${dst} 的路径索引`;
        }
        debug(`path search miss: ${key}`);
        return;
      }

      const paths = searchEntry.pathIds
        .map((pathId) => pitPathMap.get(pathId))
        .filter(Boolean)
        .map((entry) => entry.nodes);

      if (!paths.length) {
        viewer.clearPathHighlight();
        if (pathStatus) {
          pathStatus.textContent = `找到了 ${src} -> ${dst} 的路径ID，但PIT里没有对应节点序列`;
        }
        debug(`path search empty pit entries: ${key}, ids=${searchEntry.pathIds.join(" ")}`);
        return;
      }

      const ok = viewer.highlightPaths(paths);
      if (pathStatus) {
        pathStatus.textContent = ok
          ? `高亮 ${src} -> ${dst}，显示路径 ${paths.length}/${searchEntry.numPaths} 条`
          : "路径高亮失败";
      }
      debug(`path search hit: ${key}, display=${paths.length}, expected=${searchEntry.numPaths}`);
    }

    function resetPathTransparency() {
      viewer.clearPathHighlight();
      if (pathStatus) {
        pathStatus.textContent = "已恢复全部节点和链路显示";
      }
      debug("path highlight cleared");
    }

    function stepPlayback(deltaMs) {
      if (!metricsReady || !isPlaying || timestamps.length <= 1) {
        return;
      }

      const intervalMs = PLAYBACK_FRAME_INTERVAL_MS / Math.max(PLAYBACK_DEFAULT_SPEED, 0.01);
      carryMs += deltaMs;

      if (carryMs < intervalMs) {
        return;
      }

      const forward = Math.floor(carryMs / intervalMs);
      carryMs -= forward * intervalMs;

      let nextIdx = frameIdx + forward;
      if (nextIdx >= timestamps.length) {
        nextIdx = timestamps.length - 1;
        isPlaying = false;
        debug("playback reached final frame, auto pause");
      }
      renderFrame(nextIdx);
    }

    function raf(ts) {
      if (lastTs === 0) {
        lastTs = ts;
      }
      const delta = ts - lastTs;
      lastTs = ts;
      stepPlayback(delta);

      if (statusLabel && ts - lastStatusUpdateMs > 300) {
        lastStatusUpdateMs = ts;
        statusLabel.textContent = `status: ${isPlaying ? "playing" : "paused"} | frame=${frameIdx}/${timestamps.length - 1} | carryMs=${carryMs.toFixed(1)}`;
      }
      requestAnimationFrame(raf);
    }

    metricSelect.addEventListener("change", () => {
      debug(`metric changed: ${metricSelect.value}`);
      setMetricMode(metricSelect.value);
    });

    playBtn.addEventListener("click", () => {
      debug("play clicked");
      if (!metricsReady) {
        debug("play ignored: metrics not loaded");
        return;
      }
      if (timestamps.length <= 1) {
        debug("play ignored: timeline has <= 1 frame");
        return;
      }
      isPlaying = true;
    });

    pauseBtn.addEventListener("click", () => {
      debug("pause clicked");
      isPlaying = false;
    });

    slider.addEventListener("input", (e) => {
      isPlaying = false;
      carryMs = 0;
      const idx = Number(e.target.value);
      debug(`slider input -> idx=${idx}`);
      renderFrame(idx);
    });

    if (nodeQueryBtn) {
      nodeQueryBtn.addEventListener("click", queryNodeAndFocus);
    }

    if (nodeQueryInput) {
      nodeQueryInput.addEventListener("keydown", (e) => {
        if (e.key === "Enter") {
          queryNodeAndFocus();
        }
      });
    }

    if (pathFindBtn) {
      pathFindBtn.addEventListener("click", findPathsAndHighlight);
    }
    if (pathResetBtn) {
      pathResetBtn.addEventListener("click", resetPathTransparency);
    }
    if (pathSrcInput) {
      pathSrcInput.addEventListener("keydown", (e) => {
        if (e.key === "Enter") {
          findPathsAndHighlight();
        }
      });
    }
    if (pathDstInput) {
      pathDstInput.addEventListener("keydown", (e) => {
        if (e.key === "Enter") {
          findPathsAndHighlight();
        }
      });
    }

    setMetricMode(metricMode);
    renderFrame(0);
    debug("viewer ready");
    requestAnimationFrame(raf);

  } catch (error) {
    console.error(error);
    if (debugLog) {
      debugLog.textContent = String(error?.stack || error?.message || error);
    }
    statsEl.textContent = "加载失败，请检查 topology_layout.json 和 port_metrics_1us.csv 是否存在";
  }
}

bootstrap();
