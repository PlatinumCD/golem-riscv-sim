import {
  activeAt,
  clamp,
  formatCount,
  formatTime,
  routePosition,
  validateTrace,
  visibleWindow,
} from "./lib.js";

const COLORS = {
  background: "#06111c",
  grid: "rgba(110, 157, 180, 0.2)",
  gridBright: "rgba(125, 180, 203, 0.38)",
  text: "#dcecf2",
  muted: "#64818f",
  task: "#3ee8dc",
  route: "#f3c969",
  contention: "#ff6b6b",
  analog: "#c778ff",
  dma: "#66e38b",
  wait: "#7892a2",
  selected: "#ffffff",
};

const state = {
  trace: null,
  time: 0,
  playing: false,
  speed: 1,
  selectedTile: 0,
  zoom: 1,
  filters: {
    tasks: true,
    routes: true,
    analog: true,
    dma: true,
    waits: true,
  },
  lastFrame: null,
  meshGeometry: null,
};

const elements = {
  mesh: document.querySelector("#meshCanvas"),
  timeline: document.querySelector("#timelineCanvas"),
  play: document.querySelector("#playButton"),
  start: document.querySelector("#jumpStart"),
  step: document.querySelector("#stepButton"),
  slider: document.querySelector("#timeSlider"),
  speed: document.querySelector("#speedSelect"),
  currentTime: document.querySelector("#currentTime"),
  totalTime: document.querySelector("#totalTime"),
  title: document.querySelector("#datasetTitle"),
  meta: document.querySelector("#datasetMeta"),
  file: document.querySelector("#fileInput"),
  selectionTitle: document.querySelector("#selectionTitle"),
  selectionCoordinate: document.querySelector("#selectionCoordinate"),
  activityState: document.querySelector("#activityState"),
  selectionDetails: document.querySelector("#selectionDetails"),
  eventList: document.querySelector("#eventList"),
  windowLabel: document.querySelector("#windowLabel"),
  tooltip: document.querySelector("#timelineTooltip"),
};

const contexts = {
  mesh: elements.mesh.getContext("2d"),
  timeline: elements.timeline.getContext("2d"),
};

function setupCanvas(canvas, context) {
  const ratio = window.devicePixelRatio || 1;
  const rectangle = canvas.getBoundingClientRect();
  const width = Math.max(1, Math.round(rectangle.width * ratio));
  const height = Math.max(1, Math.round(rectangle.height * ratio));
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  return { width: rectangle.width, height: rectangle.height };
}

function roundedRectangle(context, x, y, width, height, radius) {
  const r = Math.min(radius, width / 2, height / 2);
  context.beginPath();
  context.roundRect(x, y, width, height, r);
}

function tilePoint(tile, geometry, trace = state.trace) {
  const x = tile % trace.meta.width;
  const y = Math.floor(tile / trace.meta.width);
  return {
    x: geometry.left + x * geometry.stepX,
    y: geometry.top + y * geometry.stepY,
  };
}

function activeCollections(time) {
  const trace = state.trace;
  return {
    tasks: state.filters.tasks ? trace.tasks.filter((event) => activeAt(event, time)) : [],
    routes: state.filters.routes ? trace.routes.filter((event) => activeAt(event, time)) : [],
    analog: state.filters.analog ? trace.analog.filter((event) => activeAt(event, time)) : [],
    dma: state.filters.dma ? trace.dma.filter((event) => activeAt(event, time)) : [],
    waits: state.filters.waits ? trace.waits.filter((event) => activeAt(event, time)) : [],
  };
}

function activityByTile(active) {
  const tiles = new Map();
  const add = (tile, kind, event) => {
    if (!tiles.has(tile)) tiles.set(tile, { tasks: [], analog: [], dma: [], waits: [], routes: [] });
    tiles.get(tile)[kind].push(event);
  };
  active.tasks.forEach((event) => add(event.tile, "tasks", event));
  active.analog.forEach((event) => add(event.tile, "analog", event));
  active.dma.forEach((event) => add(event.tile, "dma", event));
  active.waits.forEach((event) => add(event.tile, "waits", event));
  active.routes.forEach((event) => {
    add(event.source, "routes", event);
    if (event.destination !== event.source) add(event.destination, "routes", event);
  });
  return tiles;
}

function drawMesh() {
  if (!state.trace) return;
  const { width, height } = setupCanvas(elements.mesh, contexts.mesh);
  const context = contexts.mesh;
  context.clearRect(0, 0, width, height);

  const columns = state.trace.meta.width;
  const rows = state.trace.meta.height;
  const margin = Math.max(34, Math.min(width, height) * 0.075);
  const geometry = {
    left: margin,
    top: margin,
    stepX: columns > 1 ? (width - margin * 2) / (columns - 1) : 0,
    stepY: rows > 1 ? (height - margin * 2) / (rows - 1) : 0,
    tileRadius: clamp(Math.min(width / columns, height / rows) * 0.24, 11, 22),
  };
  state.meshGeometry = geometry;
  const active = activeCollections(state.time);
  const tileActivity = activityByTile(active);
  const activeLinks = new Map();

  for (const route of active.routes) {
    const position = routePosition(route, state.time);
    const key = [Math.min(position.tile, position.next), Math.max(position.tile, position.next)].join("-");
    activeLinks.set(key, (activeLinks.get(key) || 0) + 1);
  }

  context.lineCap = "round";
  for (let tile = 0; tile < columns * rows; tile += 1) {
    const x = tile % columns;
    const y = Math.floor(tile / columns);
    const point = tilePoint(tile, geometry);
    if (x + 1 < columns) {
      drawMeshLink(context, point, tilePoint(tile + 1, geometry), tile, tile + 1, activeLinks);
    }
    if (y + 1 < rows) {
      drawMeshLink(context, point, tilePoint(tile + columns, geometry), tile, tile + columns, activeLinks);
    }
  }

  for (const route of active.routes) {
    const position = routePosition(route, state.time);
    const source = tilePoint(position.tile, geometry);
    const destination = tilePoint(position.next, geometry);
    const x = source.x + (destination.x - source.x) * position.fraction;
    const y = source.y + (destination.y - source.y) * position.fraction;
    const key = [Math.min(position.tile, position.next), Math.max(position.tile, position.next)].join("-");
    const pressure = (activeLinks.get(key) || 0) > 1 || route.contended;

    context.save();
    context.shadowBlur = pressure ? 16 : 10;
    context.shadowColor = pressure ? COLORS.contention : COLORS.route;
    context.fillStyle = pressure ? COLORS.contention : COLORS.route;
    context.beginPath();
    context.arc(x, y, clamp(3 + Math.log2(Math.max(1, route.words)) * 0.33, 4, 8), 0, Math.PI * 2);
    context.fill();
    context.restore();
  }

  for (let tile = 0; tile < columns * rows; tile += 1) {
    drawTile(context, tile, geometry, tileActivity.get(tile));
  }
}

function drawMeshLink(context, start, end, source, destination, activeLinks) {
  const key = [Math.min(source, destination), Math.max(source, destination)].join("-");
  const activeCount = activeLinks.get(key) || 0;
  context.beginPath();
  context.moveTo(start.x, start.y);
  context.lineTo(end.x, end.y);
  if (activeCount > 1) {
    context.strokeStyle = COLORS.contention;
    context.lineWidth = 3.5;
    context.shadowBlur = 10;
    context.shadowColor = COLORS.contention;
  } else if (activeCount === 1) {
    context.strokeStyle = COLORS.route;
    context.lineWidth = 2.4;
    context.shadowBlur = 6;
    context.shadowColor = COLORS.route;
  } else {
    context.strokeStyle = COLORS.grid;
    context.lineWidth = 1.2;
    context.shadowBlur = 0;
  }
  context.stroke();
  context.shadowBlur = 0;
}

function tileState(activity) {
  if (!activity) return { label: "Idle", color: COLORS.muted, className: "idle-state" };
  if (activity.analog.length) return { label: "Analog compute", color: COLORS.analog, className: "analog-state" };
  if (activity.tasks.length) return { label: "Task execution", color: COLORS.task, className: "busy-state" };
  if (activity.dma.length) return { label: "Receive DMA", color: COLORS.dma, className: "dma-state" };
  if (activity.waits.length) return { label: "Blocked", color: COLORS.contention, className: "wait-state" };
  if (activity.routes.length) return { label: "Network flow", color: COLORS.route, className: "busy-state" };
  return { label: "Idle", color: COLORS.muted, className: "idle-state" };
}

function drawTile(context, tile, geometry, activity) {
  const point = tilePoint(tile, geometry);
  const radius = geometry.tileRadius;
  const selected = tile === state.selectedTile;
  const status = tileState(activity);
  context.save();
  if (activity) {
    context.shadowBlur = selected ? 22 : 13;
    context.shadowColor = status.color;
  }
  roundedRectangle(context, point.x - radius, point.y - radius, radius * 2, radius * 2, radius * 0.34);
  context.fillStyle = activity ? `${status.color}28` : "rgba(10, 28, 41, 0.96)";
  context.fill();
  context.shadowBlur = 0;
  context.strokeStyle = selected ? COLORS.selected : (activity ? status.color : COLORS.gridBright);
  context.lineWidth = selected ? 2.4 : 1.2;
  context.stroke();

  if (activity?.analog.length) {
    const phase = (state.time / Math.max(1, state.trace.meta.durationTicks)) * Math.PI * 2;
    context.beginPath();
    context.arc(point.x, point.y, radius + 5, phase, phase + Math.PI * 1.25);
    context.strokeStyle = COLORS.analog;
    context.lineWidth = 2;
    context.stroke();
  }

  context.fillStyle = selected ? COLORS.text : (activity ? status.color : COLORS.muted);
  context.font = `${Math.max(8, radius * 0.66)}px ui-monospace, SFMono-Regular, Menlo, monospace`;
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText(String(tile).padStart(2, "0"), point.x, point.y);
  context.restore();
}

function drawTimeline() {
  if (!state.trace) return;
  const { width, height } = setupCanvas(elements.timeline, contexts.timeline);
  const context = contexts.timeline;
  context.clearRect(0, 0, width, height);
  context.fillStyle = COLORS.background;
  context.fillRect(0, 0, width, height);

  const left = width < 600 ? 28 : 43;
  const right = 14;
  const top = 24;
  const bottom = 19;
  const lanes = state.trace.meta.width * state.trace.meta.height;
  const laneHeight = (height - top - bottom) / lanes;
  const window = visibleWindow(state.trace.meta.durationTicks, state.time, state.zoom);
  const span = Math.max(1, window.end - window.start);
  const xFor = (tick) => left + ((tick - window.start) / span) * (width - left - right);
  const yFor = (tile) => top + tile * laneHeight;

  context.font = "8px ui-monospace, SFMono-Regular, Menlo, monospace";
  context.textAlign = "right";
  context.textBaseline = "middle";
  for (let tile = 0; tile < lanes; tile += 1) {
    const y = yFor(tile);
    context.fillStyle = tile === state.selectedTile ? "rgba(62,232,220,0.06)" : (tile % 8 === 0 ? "rgba(255,255,255,0.015)" : "transparent");
    context.fillRect(left, y, width - left - right, laneHeight);
    context.strokeStyle = tile % 8 === 0 ? "rgba(110,157,180,0.13)" : "rgba(110,157,180,0.045)";
    context.beginPath();
    context.moveTo(left, y);
    context.lineTo(width - right, y);
    context.stroke();
    if (width >= 500 && (tile % 8 === 0 || tile === state.selectedTile)) {
      context.fillStyle = tile === state.selectedTile ? COLORS.task : COLORS.muted;
      context.fillText(String(tile), left - 6, y + laneHeight / 2);
    }
  }

  const drawIntervals = (events, color, tileOf, alpha = 0.85, minimum = 1) => {
    context.fillStyle = color;
    context.globalAlpha = alpha;
    for (const event of events) {
      if (event.end < window.start || event.start > window.end) continue;
      const tile = tileOf(event);
      const x = xFor(Math.max(window.start, event.start));
      const end = xFor(Math.min(window.end, event.end));
      context.fillRect(x, yFor(tile) + 0.5, Math.max(minimum, end - x), Math.max(1, laneHeight - 1));
    }
    context.globalAlpha = 1;
  };

  if (state.filters.waits) drawIntervals(state.trace.waits, COLORS.wait, (event) => event.tile, 0.46);
  if (state.filters.tasks) drawIntervals(state.trace.tasks, COLORS.task, (event) => event.tile, 0.9);
  if (state.filters.analog) drawIntervals(state.trace.analog, COLORS.analog, (event) => event.tile, 0.86);
  if (state.filters.dma) drawIntervals(state.trace.dma, COLORS.dma, (event) => event.tile, 0.88);
  if (state.filters.routes) drawIntervals(state.trace.routes, COLORS.route, (event) => event.source, 0.78);

  const cursorX = xFor(state.time);
  context.strokeStyle = COLORS.text;
  context.lineWidth = 1;
  context.beginPath();
  context.moveTo(cursorX, top - 8);
  context.lineTo(cursorX, height - bottom);
  context.stroke();
  context.fillStyle = COLORS.text;
  context.beginPath();
  context.moveTo(cursorX - 4, top - 8);
  context.lineTo(cursorX + 4, top - 8);
  context.lineTo(cursorX, top - 2);
  context.closePath();
  context.fill();

  context.fillStyle = COLORS.muted;
  context.textAlign = "left";
  context.textBaseline = "top";
  context.fillText(formatTime(window.start, state.trace.meta.timebasePs), left, 4);
  context.textAlign = "right";
  context.fillText(formatTime(window.end, state.trace.meta.timebasePs), width - right, 4);
}

function selectedActivity() {
  const active = activeCollections(state.time);
  return {
    tasks: active.tasks.filter((event) => event.tile === state.selectedTile),
    analog: active.analog.filter((event) => event.tile === state.selectedTile),
    dma: active.dma.filter((event) => event.tile === state.selectedTile),
    waits: active.waits.filter((event) => event.tile === state.selectedTile),
    routes: active.routes.filter(
      (event) => event.source === state.selectedTile || event.destination === state.selectedTile,
    ),
  };
}

function updateInspector() {
  if (!state.trace) return;
  const x = state.selectedTile % state.trace.meta.width;
  const y = Math.floor(state.selectedTile / state.trace.meta.width);
  const activity = selectedActivity();
  const status = tileState(activity);
  elements.selectionTitle.textContent = `Tile ${state.selectedTile}`;
  elements.selectionCoordinate.textContent = `${x}, ${y}`;
  elements.activityState.className = `activity-state ${status.className}`;
  elements.activityState.querySelector("strong").textContent = status.label;

  const allTileTasks = state.trace.tasks.filter((event) => event.tile === state.selectedTile).length;
  const allTileAnalog = state.trace.analog.filter((event) => event.tile === state.selectedTile).length;
  const ingress = state.trace.routes.filter((event) => event.destination === state.selectedTile).length;
  const egress = state.trace.routes.filter((event) => event.source === state.selectedTile).length;
  const details = [
    ["Tasks in trace", formatCount(allTileTasks)],
    ["Analog operations", formatCount(allTileAnalog)],
    ["Incoming routes", formatCount(ingress)],
    ["Outgoing routes", formatCount(egress)],
    ["Current time", formatTime(state.time, state.trace.meta.timebasePs)],
  ];
  elements.selectionDetails.innerHTML = details
    .map(([name, value]) => `<dt>${name}</dt><dd>${value}</dd>`)
    .join("");

  const entries = [];
  activity.tasks.forEach((event) => entries.push({
    className: "task-event",
    html: `<strong>Task ${event.task}</strong><br>execution ${event.execution} · ${formatCount(event.cycles)} CPU cycles`,
  }));
  activity.routes.forEach((event) => {
    const direction = event.source === state.selectedTile ? `to tile ${event.destination}` : `from tile ${event.source}`;
    entries.push({
      className: "route-event",
      html: `<strong>Route ${event.route}</strong> ${direction}<br>${formatCount(event.words)} words · ${event.hops} hops${event.contended ? " · queued" : ""}`,
    });
  });
  activity.analog.forEach((event) => entries.push({
    className: "analog-event",
    html: `<strong>${event.operation}</strong> on array ${event.array}<br>ticket ${event.ticket}`,
  }));
  activity.dma.forEach((event) => entries.push({
    className: "dma-event",
    html: `<strong>Receive DMA</strong> route ${event.route}<br>${formatCount(event.words)} words from tile ${event.source}`,
  }));
  activity.waits.forEach((event) => entries.push({
    className: "wait-event",
    html: `<strong>${event.reason}</strong><br>${formatTime(event.end - event.start, state.trace.meta.timebasePs)}`,
  }));
  elements.eventList.innerHTML = entries.length
    ? entries.slice(0, 12).map((entry) => `<div class="event-item ${entry.className}">${entry.html}</div>`).join("")
    : '<div class="empty-event">No selected activity at this timestamp.</div>';
}

function updateReadout() {
  if (!state.trace) return;
  elements.currentTime.textContent = formatTime(state.time, state.trace.meta.timebasePs);
  elements.totalTime.textContent = formatTime(state.trace.meta.durationTicks, state.trace.meta.timebasePs);
  elements.slider.value = String(Math.round((state.time / state.trace.meta.durationTicks) * 1000));
  elements.play.textContent = state.playing ? "❚❚" : "▶";
  elements.play.setAttribute("aria-label", state.playing ? "Pause" : "Play");
  elements.windowLabel.textContent = state.zoom === 1 ? "Full run" : `${state.zoom}× zoom`;
}

function render() {
  drawMesh();
  drawTimeline();
  updateInspector();
  updateReadout();
}

function loadTrace(trace) {
  state.trace = validateTrace(trace);
  state.time = 0;
  state.playing = false;
  state.selectedTile = 0;
  state.zoom = 1;
  const meta = state.trace.meta;
  const summary = state.trace.summary;
  elements.title.textContent = meta.title || "Mittens activity";
  elements.meta.textContent = `${meta.width}×${meta.height} · ${formatTime(meta.durationTicks, meta.timebasePs)} · ${meta.detail}`;
  document.querySelector("#metricTiles").textContent = formatCount(summary.activeTiles || 0);
  document.querySelector("#metricTasks").textContent = formatCount(summary.tasks || 0);
  document.querySelector("#metricRoutes").textContent = formatCount(summary.routes || 0);
  document.querySelector("#metricWords").textContent = formatCount(summary.injectedWords || 0);
  document.querySelector("#metricAnalog").textContent = formatCount(summary.analog || 0);
  document.querySelector("#metricContention").textContent = formatCount(summary.contendedRoutes || 0);
  render();
}

async function loadFromUrl() {
  const parameters = new URLSearchParams(window.location.search);
  // Some chat/Markdown clients append their closing punctuation when a query
  // parameter is copied from a rendered URL. Keep trace loading resilient to
  // that without altering valid path characters elsewhere in the URL.
  const requestedPath = parameters.get("data") || "data/sample.json";
  const path = requestedPath.replace(/[)\]}>.,;:]+$/u, "");
  const response = await fetch(path);
  if (!response.ok) throw new Error(`Could not load trace ${path}: ${response.status}`);
  loadTrace(await response.json());
}

function togglePlayback() {
  state.playing = !state.playing;
  state.lastFrame = null;
  updateReadout();
}

function seek(fraction) {
  if (!state.trace) return;
  state.time = clamp(fraction, 0, 1) * state.trace.meta.durationTicks;
  state.lastFrame = null;
  render();
}

function frame(timestamp) {
  if (state.playing && state.trace) {
    if (state.lastFrame !== null) {
      const elapsedSeconds = Math.min(0.1, (timestamp - state.lastFrame) / 1000);
      const baseTicksPerSecond = state.trace.meta.durationTicks / 30;
      state.time += elapsedSeconds * baseTicksPerSecond * state.speed;
      if (state.time >= state.trace.meta.durationTicks) {
        state.time = state.trace.meta.durationTicks;
        state.playing = false;
      }
      render();
    }
    state.lastFrame = timestamp;
  }
  requestAnimationFrame(frame);
}

elements.play.addEventListener("click", togglePlayback);
elements.start.addEventListener("click", () => seek(0));
elements.step.addEventListener("click", () => {
  if (!state.trace) return;
  state.time = Math.min(
    state.trace.meta.durationTicks,
    state.time + state.trace.meta.durationTicks / 300,
  );
  render();
});
elements.slider.addEventListener("input", (event) => seek(Number(event.target.value) / 1000));
elements.speed.addEventListener("change", (event) => {
  state.speed = Number(event.target.value);
});
elements.file.addEventListener("change", async (event) => {
  const [file] = event.target.files;
  if (!file) return;
  loadTrace(JSON.parse(await file.text()));
});
document.querySelectorAll("[data-filter]").forEach((input) => {
  input.addEventListener("change", () => {
    state.filters[input.dataset.filter] = input.checked;
    render();
  });
});
document.querySelector("#zoomIn").addEventListener("click", () => {
  state.zoom = Math.min(64, state.zoom * 2);
  render();
});
document.querySelector("#zoomOut").addEventListener("click", () => {
  state.zoom = Math.max(1, state.zoom / 2);
  render();
});

elements.mesh.addEventListener("click", (event) => {
  if (!state.trace || !state.meshGeometry) return;
  const rectangle = elements.mesh.getBoundingClientRect();
  const mouse = { x: event.clientX - rectangle.left, y: event.clientY - rectangle.top };
  let closest = null;
  for (let tile = 0; tile < state.trace.meta.width * state.trace.meta.height; tile += 1) {
    const point = tilePoint(tile, state.meshGeometry);
    const distance = Math.hypot(mouse.x - point.x, mouse.y - point.y);
    if (!closest || distance < closest.distance) closest = { tile, distance };
  }
  if (closest && closest.distance <= state.meshGeometry.tileRadius * 1.7) {
    state.selectedTile = closest.tile;
    render();
  }
});

function timelineFraction(event) {
  const rectangle = elements.timeline.getBoundingClientRect();
  const left = rectangle.width < 600 ? 28 : 43;
  const right = 14;
  const position = clamp(event.clientX - rectangle.left - left, 0, rectangle.width - left - right);
  const window = visibleWindow(state.trace.meta.durationTicks, state.time, state.zoom);
  return (window.start + (position / (rectangle.width - left - right)) * (window.end - window.start)) /
    state.trace.meta.durationTicks;
}

elements.timeline.addEventListener("click", (event) => seek(timelineFraction(event)));
elements.timeline.addEventListener("mousemove", (event) => {
  if (!state.trace) return;
  const fraction = timelineFraction(event);
  const rectangle = elements.timeline.getBoundingClientRect();
  elements.tooltip.hidden = false;
  elements.tooltip.style.left = `${event.clientX - rectangle.left}px`;
  elements.tooltip.style.top = `${event.clientY - rectangle.top}px`;
  elements.tooltip.textContent = formatTime(
    fraction * state.trace.meta.durationTicks,
    state.trace.meta.timebasePs,
  );
});
elements.timeline.addEventListener("mouseleave", () => {
  elements.tooltip.hidden = true;
});

window.addEventListener("keydown", (event) => {
  if (event.code === "Space" && !["INPUT", "SELECT"].includes(document.activeElement?.tagName)) {
    event.preventDefault();
    togglePlayback();
  }
  if (event.code === "ArrowRight" && state.trace) {
    state.time = Math.min(state.trace.meta.durationTicks, state.time + state.trace.meta.durationTicks / 300);
    render();
  }
  if (event.code === "ArrowLeft" && state.trace) {
    state.time = Math.max(0, state.time - state.trace.meta.durationTicks / 300);
    render();
  }
});
window.addEventListener("resize", render);

loadFromUrl().catch((error) => {
  elements.title.textContent = "Trace load failed";
  elements.meta.textContent = error.message;
  console.error(error);
});
requestAnimationFrame(frame);
