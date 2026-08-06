export function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

export function formatCount(value) {
  return new Intl.NumberFormat("en-US", {
    notation: value >= 1_000_000 ? "compact" : "standard",
    maximumFractionDigits: value >= 1_000_000 ? 2 : 0,
  }).format(value);
}

export function formatTime(ticks, timebasePs = 1) {
  const picoseconds = Math.max(0, ticks) * timebasePs;
  if (picoseconds < 1_000) return `${picoseconds.toFixed(0)} ps`;
  const nanoseconds = picoseconds / 1_000;
  if (nanoseconds < 1_000) return `${nanoseconds.toFixed(nanoseconds < 10 ? 2 : 1)} ns`;
  const microseconds = nanoseconds / 1_000;
  if (microseconds < 1_000) return `${microseconds.toFixed(microseconds < 10 ? 3 : 2)} µs`;
  const milliseconds = microseconds / 1_000;
  if (milliseconds < 1_000) return `${milliseconds.toFixed(milliseconds < 10 ? 4 : 3)} ms`;
  return `${(milliseconds / 1_000).toFixed(3)} s`;
}

export function activeAt(event, time) {
  return event.start <= time && event.end >= time;
}

export function routePosition(route, time) {
  if (!route.path?.length) return { tile: route.source, next: route.source, fraction: 0 };
  if (route.path.length === 1 || route.end <= route.start) {
    return { tile: route.path[0], next: route.path[0], fraction: 0 };
  }
  const progress = clamp((time - route.start) / (route.end - route.start), 0, 0.999999);
  const scaled = progress * (route.path.length - 1);
  const index = Math.floor(scaled);
  return {
    tile: route.path[index],
    next: route.path[index + 1],
    fraction: scaled - index,
    segment: index,
  };
}

export function visibleWindow(duration, current, zoom) {
  const span = duration / Math.max(1, zoom);
  const half = span / 2;
  let start = current - half;
  let end = current + half;
  if (start < 0) {
    end -= start;
    start = 0;
  }
  if (end > duration) {
    start -= end - duration;
    end = duration;
  }
  return {
    start: Math.max(0, start),
    end: Math.max(span, end),
  };
}

export function validateTrace(trace) {
  if (!trace || ![1, 2].includes(trace.schemaVersion)) {
    throw new Error("Unsupported or missing visualization schema");
  }
  const width = Number(trace.meta?.width);
  const height = Number(trace.meta?.height);
  const duration = Number(trace.meta?.durationTicks);
  if (!Number.isInteger(width) || width <= 0 ||
      !Number.isInteger(height) || height <= 0 ||
      !Number.isFinite(duration) || duration <= 0) {
    throw new Error("Trace contains invalid mesh dimensions or duration");
  }
  for (const name of [
    "tasks",
    "routes",
    "packets",
    "dma",
    "analog",
    "waits",
    "blocked",
    "links",
  ]) {
    if (!Array.isArray(trace[name])) trace[name] = [];
  }
  return trace;
}
