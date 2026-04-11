# Stage 2 - Three.js 最小可运行骨架

本阶段只实现：
- 3D 拓扑展示
- 鼠标拖动/缩放/旋转
- 从 topology_layout.json 加载节点与链路

未实现（后续阶段）：
- 时间回放
- 多指标切换（queue/rate/congestion）

## 运行方式

在当前目录执行：

```bash
python3 -m http.server 8080
```

浏览器打开：

```text
http://127.0.0.1:8080/
```

## 目录

- index.html: 页面入口
- src/main.js: 启动入口
- src/data-loader.js: 布局数据读取
- src/topology-scene.js: Three.js 场景与绘制逻辑
- src/config.js: 可视化配置
- src/styles.css: 样式
