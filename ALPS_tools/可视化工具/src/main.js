import {
  getPortMetric,
  loadPortMetricsTimeline,
  loadTopologyLayout,
} from "./data-loader.js";
import { PLAYBACK_DEFAULT_SPEED, PLAYBACK_FRAME_INTERVAL_MS } from "./config.js";
import { createTopologyViewer } from "./topology-scene.js";

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

    viewer.setNodeSelectHandler((node) => {
      nodeInfo.textContent = `节点 ${node.id} | 类型 ${node.type} | 位置 (${node.x}, ${node.y}, ${node.z})`;
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
    }

    function setMetricMode(mode) {
      metricMode = mode === "utilization" ? "utilization" : "queue";
      viewer.setMetricMode(metricMode);
      renderFrame(frameIdx);
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
