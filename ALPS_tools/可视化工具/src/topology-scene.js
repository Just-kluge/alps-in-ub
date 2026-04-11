import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import {
  EDGE_BASE_RADIUS,
  EDGE_RADIUS_GAIN,
  NODE_COLORS,
  NODE_SIZE,
  QUEUE_LIMIT_BYTES,
  UTILIZATION_MAX,
} from "./config.js";

function addAxes(scene) {
  const axes = new THREE.AxesHelper(8);
  scene.add(axes);
}

function addGrid(scene) {
  const grid = new THREE.GridHelper(180, 36, 0x8d7b68, 0xb8aa96);
  grid.position.set(24, 6, 0);
  scene.add(grid);
}

function buildNodeMeshes(scene, nodes) {
  const groups = {
    Host: [],
    ToR: [],
    DU: [],
    L1: [],
    HRS: [],
  };

  nodes.forEach((node) => {
    groups[node.type].push(node);
  });

  const instanceGroups = [];

  Object.keys(groups).forEach((type) => {
    const list = groups[type];
    if (!list.length) {
      return;
    }

    const geometry = new THREE.SphereGeometry(NODE_SIZE[type], 10, 10);
    const material = new THREE.MeshStandardMaterial({
      color: NODE_COLORS[type],
      roughness: 0.35,
      metalness: 0.05,
    });

    const mesh = new THREE.InstancedMesh(geometry, material, list.length);
    const matrix = new THREE.Matrix4();
    list.forEach((node, i) => {
      matrix.makeTranslation(node.x, node.z, node.y);
      mesh.setMatrixAt(i, matrix);
    });
    mesh.instanceMatrix.needsUpdate = true;
    scene.add(mesh);
    instanceGroups.push({ type, nodes: list, mesh });
  });

  return instanceGroups;
}

function toWorldPos(node) {
  return new THREE.Vector3(node.x, node.z, node.y);
}

function buildHalfEdges(edges, nodeMap) {
  const halfEdges = [];
  edges.forEach((edge) => {
    const src = nodeMap.get(edge.src);
    const dst = nodeMap.get(edge.dst);
    if (!src || !dst) {
      return;
    }

    const srcPos = toWorldPos(src);
    const dstPos = toWorldPos(dst);
    const midPos = srcPos.clone().add(dstPos).multiplyScalar(0.5);

    halfEdges.push({
      from: srcPos,
      to: midPos,
      nodeId: edge.src,
      portId: edge.srcPort,
    });

    halfEdges.push({
      from: dstPos,
      to: midPos,
      nodeId: edge.dst,
      portId: edge.dstPort,
    });
  });
  return halfEdges;
}

function buildHalfEdgeMesh(scene, halfEdges) {
  const geometry = new THREE.CylinderGeometry(1, 1, 1, 6, 1, false);
  const material = new THREE.MeshStandardMaterial({
    color: 0xffffff,
    roughness: 0.55,
    metalness: 0.05,
  });

  const mesh = new THREE.InstancedMesh(geometry, material, halfEdges.length);
  mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  scene.add(mesh);
  return mesh;
}

function normQueue(queueBytes) {
  return Math.min(Math.max(queueBytes / QUEUE_LIMIT_BYTES, 0), 1);
}

function normUtil(utilization) {
  return Math.min(Math.max(utilization / UTILIZATION_MAX, 0), 1);
}

function lerpColor(a, b, t) {
  return {
    r: a.r + (b.r - a.r) * t,
    g: a.g + (b.g - a.g) * t,
    b: a.b + (b.b - a.b) * t,
  };
}

function gradient3(n, c0, c1, c2) {
  if (n <= 0.5) {
    return lerpColor(c0, c1, n * 2);
  }
  return lerpColor(c1, c2, (n - 0.5) * 2);
}

function queueColor(n) {
  return gradient3(
    n,
    { r: 0.14, g: 0.76, b: 0.37 },
    { r: 0.97, g: 0.84, b: 0.18 },
    { r: 0.92, g: 0.24, b: 0.24 }
  );
}

function utilColor(n) {
  return gradient3(
    n,
    { r: 0.92, g: 0.24, b: 0.24 },
    { r: 0.97, g: 0.84, b: 0.18 },
    { r: 0.14, g: 0.76, b: 0.37 }
  );
}

function resolveMetricVisual(metricMode, metric) {
  const fallback = {
    radius: EDGE_BASE_RADIUS,
    color: { r: 0.56, g: 0.56, b: 0.56 },
    norm: 0,
    raw: 0,
  };

  if (!metric) {
    return fallback;
  }

  if (metricMode === "queue") {
    const n = normQueue(metric.queueBytes || 0);
    return {
      radius: EDGE_BASE_RADIUS + EDGE_RADIUS_GAIN * n,
      color: queueColor(n),
      norm: n,
      raw: metric.queueBytes || 0,
    };
  }

  const n = normUtil(metric.bandwidthUtilization || 0);
  return {
    radius: EDGE_BASE_RADIUS + EDGE_RADIUS_GAIN * n,
    color: utilColor(n),
    norm: n,
    raw: metric.bandwidthUtilization || 0,
  };
}

function buildEdgeUpdater(halfEdges, halfEdgeMesh) {
  const tempMatrix = new THREE.Matrix4();
  const tempQuat = new THREE.Quaternion();
  const tempScale = new THREE.Vector3();
  const tempPos = new THREE.Vector3();
  const up = new THREE.Vector3(0, 1, 0);
  const tempColor = new THREE.Color(0xffffff);

  return (metricMode, metricResolver) => {
    const summary = {
      samples: 0,
      queueMax: 0,
      utilMax: 0,
    };

    halfEdges.forEach((half, i) => {
      const dir = half.to.clone().sub(half.from);
      const length = Math.max(dir.length(), 1e-6);
      dir.normalize();

      const metric = metricResolver(half.nodeId, half.portId);
      if (metric) {
        summary.samples += 1;
        summary.queueMax = Math.max(summary.queueMax, metric.queueBytes || 0);
        summary.utilMax = Math.max(summary.utilMax, metric.bandwidthUtilization || 0);
      }

      const visual = resolveMetricVisual(metricMode, metric);

      tempPos.copy(half.from).add(half.to).multiplyScalar(0.5);
      tempQuat.setFromUnitVectors(up, dir);
      tempScale.set(visual.radius, length, visual.radius);
      tempMatrix.compose(tempPos, tempQuat, tempScale);

      halfEdgeMesh.setMatrixAt(i, tempMatrix);
      tempColor.setRGB(visual.color.r, visual.color.g, visual.color.b);
      halfEdgeMesh.setColorAt(i, tempColor);
    });

    halfEdgeMesh.instanceMatrix.needsUpdate = true;
    if (halfEdgeMesh.instanceColor) {
      halfEdgeMesh.instanceColor.needsUpdate = true;
    }

    return summary;
  };
}

export function createTopologyViewer(canvas, layout) {
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0xf9f4ea);

  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));

  const camera = new THREE.PerspectiveCamera(55, 1, 0.1, 2000);
  camera.position.set(35, 40, 95);

  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.target.set(24, 7, 8);
  controls.update();

  const hemi = new THREE.HemisphereLight(0xfff6e8, 0x705f4f, 1.1);
  scene.add(hemi);

  const dir = new THREE.DirectionalLight(0xffffff, 0.75);
  dir.position.set(40, 55, 20);
  scene.add(dir);

  addAxes(scene);
  addGrid(scene);

  const nodeMap = new Map(layout.nodes.map((n) => [n.id, n]));
  const nodeGroups = buildNodeMeshes(scene, layout.nodes);
  const halfEdges = buildHalfEdges(layout.edges, nodeMap);
  const halfEdgeMesh = buildHalfEdgeMesh(scene, halfEdges);
  const updateHalfEdges = buildEdgeUpdater(halfEdges, halfEdgeMesh);

  let metricMode = "queue";
  let onNodeSelect = null;

  const raycaster = new THREE.Raycaster();
  const pointer = new THREE.Vector2();

  function pickNode(event) {
    const rect = canvas.getBoundingClientRect();
    pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
    pointer.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;

    raycaster.setFromCamera(pointer, camera);
    const meshes = nodeGroups.map((g) => g.mesh);
    const hits = raycaster.intersectObjects(meshes, false);
    if (!hits.length) {
      return;
    }

    const hit = hits[0];
    const group = nodeGroups.find((g) => g.mesh === hit.object);
    if (!group || hit.instanceId == null) {
      return;
    }

    const node = group.nodes[hit.instanceId];
    if (node && typeof onNodeSelect === "function") {
      onNodeSelect(node);
    }
  }

  canvas.addEventListener("click", pickNode);

  const emptySummary = updateHalfEdges(metricMode, () => null);

  function onResize() {
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    camera.aspect = width / Math.max(height, 1);
    camera.updateProjectionMatrix();
    renderer.setSize(width, height, false);
  }

  onResize();
  window.addEventListener("resize", onResize);

  function animate() {
    controls.update();
    renderer.render(scene, camera);
    requestAnimationFrame(animate);
  }

  animate();

  return {
    setMetricMode(mode) {
      metricMode = mode === "utilization" ? "utilization" : "queue";
    },
    setFrameMetricResolver(metricResolver) {
      return updateHalfEdges(metricMode, metricResolver);
    },
    setNodeSelectHandler(handler) {
      onNodeSelect = handler;
    },
    getEmptySummary() {
      return emptySummary;
    },
  };
}
