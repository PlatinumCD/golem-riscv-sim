(() => {
  "use strict";

  const data = window.GPT2_SWEEP_DATA;
  const svgNamespace = "http://www.w3.org/2000/svg";
  const modeColors = {
    analog: "#4da6ff",
    digital: "#ff9c42",
  };
  const tokenColors = {
    4: "#86c5ff",
    8: "#4ed6a1",
    16: "#ffd166",
    32: "#ff7b89",
  };
  const schedulerColors = [
    "#78b9ff", "#ffad66", "#5fddaa", "#d39cff", "#ff7f91",
    "#69d2e7", "#f5d76e", "#9bb7ff", "#ff92cf", "#72df7e",
    "#c7a77a", "#8ce0d1", "#b69cff", "#f28f6b",
  ];
  const dashPatterns = [
    "", "8 5", "2 5", "12 5 2 5", "5 4", "10 4", "2 3 8 3",
    "14 5", "3 4 3 8", "9 3 2 3", "1 4", "12 3 3 3",
    "6 3 1 3", "15 4 4 4",
  ];

  if (!data || !Array.isArray(data.results)) {
    document.querySelector("#run-status-text").textContent =
      "Sweep data unavailable";
    return;
  }

  const state = {
    configurations: new Set(data.configurations.map((item) => item.id)),
    modes: new Set(data.modes),
  };
  const configurationById = new Map(
    data.configurations.map((configuration, index) => [
      configuration.id,
      { ...configuration, index },
    ]),
  );
  const results = data.results
    .filter((result) => result.status === "pass" && Number.isFinite(result.runtimeNs))
    .map((result) => ({
      ...result,
      runtimeMs: result.runtimeNs / 1e6,
      instructionBillions: result.instructions / 1e9,
      transferMegabytes: result.transferBytes / 1e6,
      maxTileMs: result.maxTileCycles / 1e6,
      vectorFraction:
        result.instructions > 0 ? result.vectorInstructions / result.instructions : 0,
    }));
  const resultByKey = new Map(
    results.map((result) => [
      `${result.tokens}|${result.configuration}|${result.mode}`,
      result,
    ]),
  );

  for (const result of results) {
    const peers = results.filter((peer) =>
      peer.tokens === result.tokens && peer.mode === result.mode);
    result.normalizedRuntime =
      result.runtimeNs / Math.min(...peers.map((peer) => peer.runtimeNs));
    result.normalizedGraphScore =
      result.graphScore / Math.min(...peers.map((peer) => peer.graphScore));
    result.normalizedInstructions =
      result.instructions / Math.min(...peers.map((peer) => peer.instructions));
    result.normalizedTransfer =
      result.transferBytes / Math.min(...peers.map((peer) => peer.transferBytes));
  }

  const tooltip = document.querySelector("#tooltip");
  const tooltipLabel = document.querySelector("#tooltip-label");
  const tooltipValue = document.querySelector("#tooltip-value");
  const tooltipDetails = document.querySelector("#tooltip-details");
  const schedulerList = document.querySelector("#scheduler-list");

  function svgElement(name, attributes = {}) {
    const element = document.createElementNS(svgNamespace, name);
    for (const [key, value] of Object.entries(attributes)) {
      if (value !== null && value !== undefined && value !== "") {
        element.setAttribute(key, String(value));
      }
    }
    return element;
  }

  function addText(parent, value, attributes = {}) {
    const element = svgElement("text", attributes);
    element.textContent = value;
    parent.append(element);
    return element;
  }

  function polygonPath(sides, radius = 7, rotation = -Math.PI / 2) {
    return Array.from({ length: sides }, (_, index) => {
      const angle = rotation + (Math.PI * 2 * index) / sides;
      const x = Math.cos(angle) * radius;
      const y = Math.sin(angle) * radius;
      return `${index === 0 ? "M" : "L"}${x.toFixed(2)},${y.toFixed(2)}`;
    }).join(" ") + " Z";
  }

  function starPath(points = 5, outer = 8, inner = 3.7) {
    return Array.from({ length: points * 2 }, (_, index) => {
      const angle = -Math.PI / 2 + (Math.PI * index) / points;
      const radius = index % 2 === 0 ? outer : inner;
      const x = Math.cos(angle) * radius;
      const y = Math.sin(angle) * radius;
      return `${index === 0 ? "M" : "L"}${x.toFixed(2)},${y.toFixed(2)}`;
    }).join(" ") + " Z";
  }

  const markerPaths = [
    "M0,-7 A7,7 0 1,1 0,7 A7,7 0 1,1 0,-7 Z",
    "M-6.5,-6.5 H6.5 V6.5 H-6.5 Z",
    "M0,-8 L8,0 L0,8 L-8,0 Z",
    polygonPath(3, 8),
    polygonPath(3, 8, Math.PI / 2),
    polygonPath(5, 7.7),
    polygonPath(6, 7.5),
    starPath(),
    "M-2.4,-8 H2.4 V-2.4 H8 V2.4 H2.4 V8 H-2.4 V2.4 H-8 V-2.4 H-2.4 Z",
    "M-6.2,-8 L0,-2.2 L6.2,-8 L8,-6.2 L2.2,0 L8,6.2 L6.2,8 L0,2.2 L-6.2,8 L-8,6.2 L-2.2,0 L-8,-6.2 Z",
    "M-8,-6 L0,-1.8 L8,-6 L4,0 L8,6 L0,1.8 L-8,6 L-4,0 Z",
    "M0,-8 L6,-2 L3,8 L-3,8 L-6,-2 Z",
    polygonPath(8, 7.5),
    "M0,-9 L6,-1 L3,8 L0,5 L-3,8 L-6,-1 Z",
  ];

  function formatRuntime(milliseconds) {
    if (!Number.isFinite(milliseconds)) return "—";
    if (milliseconds >= 1000) return `${(milliseconds / 1000).toFixed(3)} s`;
    if (milliseconds >= 100) return `${milliseconds.toFixed(1)} ms`;
    return `${milliseconds.toFixed(2)} ms`;
  }

  function formatCompact(value, digits = 2) {
    if (!Number.isFinite(value)) return "—";
    return new Intl.NumberFormat("en-US", {
      notation: "compact",
      maximumFractionDigits: digits,
    }).format(value);
  }

  function formatPercent(value, digits = 1) {
    return `${value.toFixed(digits)}%`;
  }

  function average(values) {
    return values.length
      ? values.reduce((sum, value) => sum + value, 0) / values.length
      : NaN;
  }

  function resultFor(tokens, configuration, mode) {
    return resultByKey.get(`${tokens}|${configuration}|${mode}`);
  }

  function selectedConfigurations() {
    return data.configurations
      .filter((configuration) => state.configurations.has(configuration.id))
      .map((configuration) => configurationById.get(configuration.id));
  }

  function selectedResults() {
    return results.filter((result) =>
      state.configurations.has(result.configuration) &&
      state.modes.has(result.mode));
  }

  function pairedResults() {
    const pairs = [];
    for (const configuration of selectedConfigurations()) {
      for (const tokens of data.tokens) {
        const analog = resultFor(tokens, configuration.id, "analog");
        const digital = resultFor(tokens, configuration.id, "digital");
        if (analog && digital) pairs.push({ configuration, tokens, analog, digital });
      }
    }
    return pairs;
  }

  function configurationLabel(id) {
    return configurationById.get(id)?.label ?? id;
  }

  function showTooltip(event, title, value, details = []) {
    tooltipLabel.textContent = title;
    tooltipValue.textContent = value;
    tooltipDetails.replaceChildren();
    for (const [name, detail] of details) {
      const term = document.createElement("dt");
      const description = document.createElement("dd");
      term.textContent = name;
      description.textContent = String(detail);
      tooltipDetails.append(term, description);
    }
    const x = event?.clientX ?? window.innerWidth / 2;
    const y = event?.clientY ?? window.innerHeight / 2;
    tooltip.style.left = `${Math.min(window.innerWidth - 300, Math.max(10, x))}px`;
    tooltip.style.top = `${Math.max(130, y)}px`;
    tooltip.classList.add("visible");
  }

  function hideTooltip() {
    tooltip.classList.remove("visible");
  }

  function bindTooltip(element, title, value, details = []) {
    element.setAttribute("tabindex", "0");
    const reveal = (event) => showTooltip(event, title, value, details);
    element.addEventListener("mouseenter", reveal);
    element.addEventListener("mousemove", reveal);
    element.addEventListener("focus", (event) => {
      const bounds = event.currentTarget.getBoundingClientRect();
      reveal({ clientX: bounds.left + bounds.width / 2, clientY: bounds.top });
    });
    element.addEventListener("mouseleave", hideTooltip);
    element.addEventListener("blur", hideTooltip);
  }

  function renderMarker(parent, index, x, y, color, scale = 1) {
    const marker = svgElement("path", {
      d: markerPaths[index % markerPaths.length],
      transform: `translate(${x} ${y}) scale(${scale})`,
      fill: color,
      class: "series-marker",
    });
    marker.style.color = color;
    parent.append(marker);
    return marker;
  }

  function renderPoint(parent, point) {
    const hit = svgElement("circle", {
      cx: point.x,
      cy: point.y,
      r: Math.max(11, (point.radius ?? 6) + 5),
      class: "point-hit",
      role: "button",
      "aria-label": `${point.title}: ${point.value}`,
    });
    bindTooltip(hit, point.title, point.value, point.details);
    parent.append(hit);
    renderMarker(
      parent,
      point.configurationIndex ?? 0,
      point.x,
      point.y,
      point.color,
      (point.radius ?? 6) / 7,
    );
  }

  function numericDomain(values, options = {}) {
    const finite = values.filter(Number.isFinite);
    if (!finite.length) return [0, 1];
    let minimum = Math.min(...finite);
    let maximum = Math.max(...finite);
    if (options.zero) minimum = Math.min(0, minimum);
    if (minimum === maximum) {
      const delta = Math.abs(minimum || 1) * 0.1;
      return [minimum - delta, maximum + delta];
    }
    const padding = (maximum - minimum) * (options.padding ?? 0.08);
    return [
      options.zero && minimum === 0 ? 0 : minimum - padding,
      maximum + padding,
    ];
  }

  function linearScale(domain, range) {
    const denominator = domain[1] - domain[0] || 1;
    return (value) =>
      range[0] + ((value - domain[0]) / denominator) * (range[1] - range[0]);
  }

  function ticks(domain, count = 5) {
    return Array.from({ length: count + 1 }, (_, index) =>
      domain[0] + ((domain[1] - domain[0]) * index) / count);
  }

  function drawAxes(svg, options) {
    const width = options.width;
    const height = options.height;
    const margin = options.margin ?? { top: 26, right: 24, bottom: 58, left: 72 };
    const x = linearScale(options.xDomain, [margin.left, width - margin.right]);
    const y = linearScale(options.yDomain, [height - margin.bottom, margin.top]);
    const xTicks = options.xTicks ?? ticks(options.xDomain);
    const yTicks = options.yTicks ?? ticks(options.yDomain);
    const group = svgElement("g", { "aria-hidden": "true" });

    for (const value of yTicks) {
      const position = y(value);
      group.append(svgElement("line", {
        x1: margin.left,
        x2: width - margin.right,
        y1: position,
        y2: position,
        class: "grid-line",
      }));
      addText(group, options.yFormat?.(value) ?? value.toFixed(1), {
        x: margin.left - 11,
        y: position + 4,
        class: "tick-label",
        "text-anchor": "end",
      });
    }
    for (const value of xTicks) {
      const position = x(value);
      group.append(svgElement("line", {
        x1: position,
        x2: position,
        y1: margin.top,
        y2: height - margin.bottom,
        class: "grid-line",
      }));
      addText(group, options.xFormat?.(value) ?? value.toFixed(1), {
        x: position,
        y: height - margin.bottom + 24,
        class: "tick-label",
        "text-anchor": "middle",
      });
    }
    group.append(svgElement("line", {
      x1: margin.left,
      x2: width - margin.right,
      y1: height - margin.bottom,
      y2: height - margin.bottom,
      class: "axis-line",
    }));
    group.append(svgElement("line", {
      x1: margin.left,
      x2: margin.left,
      y1: margin.top,
      y2: height - margin.bottom,
      class: "axis-line",
    }));
    if (options.xLabel) {
      addText(group, options.xLabel, {
        x: margin.left + (width - margin.left - margin.right) / 2,
        y: height - 12,
        class: "axis-label",
        "text-anchor": "middle",
      });
    }
    if (options.yLabel) {
      addText(group, options.yLabel, {
        x: 16,
        y: margin.top + (height - margin.top - margin.bottom) / 2,
        class: "axis-label",
        "text-anchor": "middle",
        transform:
          `rotate(-90 16 ${margin.top + (height - margin.top - margin.bottom) / 2})`,
      });
    }
    svg.append(group);
    return { x, y, margin };
  }

  function emptyChart(svg, width, height, message = "Select at least one scheduler.") {
    svg.replaceChildren();
    addText(svg, message, {
      x: width / 2,
      y: height / 2,
      class: "empty-chart",
    });
  }

  function scatterChart(id, points, options) {
    const svg = document.querySelector(id);
    const width = options.width ?? 760;
    const height = options.height ?? 430;
    svg.replaceChildren();
    if (!points.length) {
      emptyChart(svg, width, height);
      return;
    }
    const xDomain = options.xDomain ?? numericDomain(points.map((point) => point.x), {
      zero: options.xZero,
      padding: options.xPadding,
    });
    const yDomain = options.yDomain ?? numericDomain(points.map((point) => point.y), {
      zero: options.yZero,
      padding: options.yPadding,
    });
    const axes = drawAxes(svg, {
      width,
      height,
      xDomain,
      yDomain,
      xTicks: options.xTicks,
      yTicks: options.yTicks,
      xFormat: options.xFormat,
      yFormat: options.yFormat,
      xLabel: options.xLabel,
      yLabel: options.yLabel,
      margin: options.margin,
    });
    if (options.referenceY !== undefined) {
      const y = axes.y(options.referenceY);
      svg.append(svgElement("line", {
        x1: axes.margin.left,
        x2: width - axes.margin.right,
        y1: y,
        y2: y,
        class: "reference-line",
      }));
    }
    if (options.referenceX !== undefined) {
      const x = axes.x(options.referenceX);
      svg.append(svgElement("line", {
        x1: x,
        x2: x,
        y1: axes.margin.top,
        y2: height - axes.margin.bottom,
        class: "reference-line",
      }));
    }
    const group = svgElement("g");
    for (const point of points) {
      renderPoint(group, {
        ...point,
        x: axes.x(point.x),
        y: axes.y(point.y),
      });
    }
    svg.append(group);
  }

  function lineChart(id, series, options) {
    const svg = document.querySelector(id);
    const width = options.width ?? 760;
    const height = options.height ?? 430;
    svg.replaceChildren();
    const values = series.flatMap((item) => item.points.map((point) => point.value));
    if (!values.length) {
      emptyChart(svg, width, height);
      return;
    }
    const xDomain = [Math.min(...data.tokens), Math.max(...data.tokens)];
    const yDomain = options.yDomain ?? numericDomain(values, {
      zero: options.zero,
      padding: 0.1,
    });
    const axes = drawAxes(svg, {
      width,
      height,
      xDomain,
      yDomain,
      xTicks: data.tokens,
      yTicks: options.yTicks,
      xFormat: (value) => String(Math.round(value)),
      yFormat: options.yFormat,
      xLabel: "TOKEN COUNT",
      yLabel: options.yLabel,
    });
    if (options.referenceY !== undefined) {
      const y = axes.y(options.referenceY);
      svg.append(svgElement("line", {
        x1: axes.margin.left,
        x2: width - axes.margin.right,
        y1: y,
        y2: y,
        class: "reference-line",
      }));
    }
    for (const item of series) {
      const group = svgElement("g", { class: "series-group" });
      const ordered = [...item.points].sort((left, right) => left.tokens - right.tokens);
      const path = ordered.map((point, index) =>
        `${index ? "L" : "M"}${axes.x(point.tokens)},${axes.y(point.value)}`,
      ).join(" ");
      group.append(svgElement("path", {
        d: path,
        class: "series-line",
        stroke: item.color,
        "stroke-dasharray": item.dash ?? "",
      }));
      for (const point of ordered) {
        renderPoint(group, {
          x: axes.x(point.tokens),
          y: axes.y(point.value),
          color: item.color,
          configurationIndex: item.configurationIndex,
          radius: 6,
          title: point.title,
          value: point.displayValue,
          details: point.details,
        });
      }
      svg.append(group);
    }
  }

  function heatColor(value, maximum) {
    const clamped = Math.max(0, Math.min(1, (value - 1) / (maximum - 1 || 1)));
    const start = [52, 151, 125];
    const middle = [192, 156, 68];
    const end = [190, 72, 89];
    const interpolate = (left, right, amount) =>
      left.map((component, index) =>
        Math.round(component + (right[index] - component) * amount));
    const rgb = clamped < 0.5
      ? interpolate(start, middle, clamped * 2)
      : interpolate(middle, end, (clamped - 0.5) * 2);
    return `rgb(${rgb.join(",")})`;
  }

  function renderHeatmap() {
    const svg = document.querySelector("#chart-normalized-heatmap");
    const width = 1180;
    const height = 710;
    svg.replaceChildren();
    const configurations = selectedConfigurations();
    const modes = data.modes.filter((mode) => state.modes.has(mode));
    if (!configurations.length || !modes.length) {
      emptyChart(svg, width, height);
      return;
    }
    const values = results
      .filter((result) =>
        state.configurations.has(result.configuration) &&
        state.modes.has(result.mode))
      .map((result) => result.normalizedRuntime);
    const maximum = Math.max(1.01, ...values);
    const labelWidth = 255;
    const panelGap = 48;
    const available = width - labelWidth - 35 - panelGap * (modes.length - 1);
    const panelWidth = available / modes.length;
    const cellWidth = Math.min(88, (panelWidth - 15) / data.tokens.length);
    const rowHeight = Math.min(39, 535 / Math.max(1, configurations.length));
    const startY = 96;

    configurations.forEach((configuration, row) => {
      const y = startY + row * rowHeight;
      addText(svg, configuration.label, {
        x: labelWidth - 13,
        y: y + rowHeight / 2 + 4,
        class: "heatmap-label",
        "text-anchor": "end",
        "font-size": configurations.length > 10 ? 9 : 11,
      });
    });

    modes.forEach((mode, panelIndex) => {
      const panelX = labelWidth + panelIndex * (panelWidth + panelGap);
      addText(svg, mode.toUpperCase(), {
        x: panelX + (cellWidth * data.tokens.length) / 2,
        y: 31,
        class: "panel-label",
        "text-anchor": "middle",
        fill: modeColors[mode],
      });
      data.tokens.forEach((token, column) => {
        addText(svg, `${token} TOKENS`, {
          x: panelX + column * cellWidth + cellWidth / 2,
          y: 65,
          class: "tick-label",
          "text-anchor": "middle",
        });
      });

      configurations.forEach((configuration, row) => {
        data.tokens.forEach((token, column) => {
          const result = resultFor(token, configuration.id, mode);
          if (!result) return;
          const x = panelX + column * cellWidth;
          const y = startY + row * rowHeight;
          const cell = svgElement("rect", {
            x,
            y,
            width: cellWidth,
            height: rowHeight,
            rx: 4,
            fill: heatColor(result.normalizedRuntime, maximum),
            class: "heatmap-cell",
            role: "button",
          });
          bindTooltip(
            cell,
            `${configuration.label} · ${mode}`,
            `${result.normalizedRuntime.toFixed(3)}× regret`,
            [
              ["Tokens", token],
              ["Runtime", formatRuntime(result.runtimeMs)],
              ["Best", formatRuntime(result.runtimeMs / result.normalizedRuntime)],
            ],
          );
          svg.append(cell);
          addText(svg, `${result.normalizedRuntime.toFixed(2)}×`, {
            x: x + cellWidth / 2,
            y: y + rowHeight / 2,
            class: "heatmap-value",
            "text-anchor": "middle",
          });
          addText(svg, formatRuntime(result.runtimeMs), {
            x: x + cellWidth / 2,
            y: y + rowHeight / 2 + 11,
            class: "heatmap-runtime",
            "text-anchor": "middle",
          });
        });
      });
    });

    const legendX = labelWidth;
    const legendY = height - 38;
    for (let index = 0; index < 80; index += 1) {
      svg.append(svgElement("rect", {
        x: legendX + index * 3,
        y: legendY,
        width: 3,
        height: 9,
        fill: heatColor(1 + ((maximum - 1) * index) / 79, maximum),
      }));
    }
    addText(svg, "1.00× best", {
      x: legendX,
      y: legendY + 25,
      class: "tick-label",
    });
    addText(svg, `${maximum.toFixed(2)}× slowest`, {
      x: legendX + 240,
      y: legendY + 25,
      class: "tick-label",
      "text-anchor": "end",
    });
  }

  function renderSpeedup() {
    const series = selectedConfigurations().map((configuration) => ({
      color: schedulerColors[configuration.index],
      dash: dashPatterns[configuration.index],
      configurationIndex: configuration.index,
      points: data.tokens.map((tokens) => {
        const analog = resultFor(tokens, configuration.id, "analog");
        const digital = resultFor(tokens, configuration.id, "digital");
        const value = digital.runtimeNs / analog.runtimeNs;
        return {
          tokens,
          value,
          title: configuration.label,
          displayValue: `${value.toFixed(2)}× analog speedup`,
          details: [
            ["Tokens", tokens],
            ["Analog", formatRuntime(analog.runtimeMs)],
            ["Digital", formatRuntime(digital.runtimeMs)],
          ],
        };
      }),
    }));
    lineChart("#chart-speedup", series, {
      yLabel: "DIGITAL ÷ ANALOG",
      yFormat: (value) => `${value.toFixed(1)}×`,
      referenceY: 1,
    });
  }

  function renderScaling() {
    const series = [];
    for (const configuration of selectedConfigurations()) {
      for (const mode of data.modes) {
        if (!state.modes.has(mode)) continue;
        series.push({
          color: modeColors[mode],
          dash: dashPatterns[configuration.index],
          configurationIndex: configuration.index,
          points: data.tokens.map((tokens) => {
            const result = resultFor(tokens, configuration.id, mode);
            return {
              tokens,
              value: result.runtimeMs,
              title: `${configuration.label} · ${mode}`,
              displayValue: formatRuntime(result.runtimeMs),
              details: [
                ["Tokens", tokens],
                ["Instructions", formatCompact(result.instructions)],
              ],
            };
          }),
        });
      }
    }
    lineChart("#chart-scaling", series, {
      yLabel: "SIMULATED RUNTIME",
      yFormat: (value) => formatRuntime(value),
      zero: true,
    });
  }

  function modeRanks(mode) {
    return data.configurations
      .map((configuration) => ({
        configuration: configurationById.get(configuration.id),
        score: average(
          data.tokens.map((tokens) =>
            resultFor(tokens, configuration.id, mode).normalizedRuntime),
        ),
      }))
      .sort((left, right) => left.score - right.score)
      .map((item, index) => ({ ...item, rank: index + 1 }));
  }

  function renderRankMigration() {
    const svg = document.querySelector("#chart-rank");
    const width = 1180;
    const height = 620;
    svg.replaceChildren();
    const selected = selectedConfigurations();
    if (!selected.length) {
      emptyChart(svg, width, height);
      return;
    }
    const analogRanks = modeRanks("analog");
    const digitalRanks = modeRanks("digital");
    const analogById = new Map(
      analogRanks.map((item) => [item.configuration.id, item]),
    );
    const digitalById = new Map(
      digitalRanks.map((item) => [item.configuration.id, item]),
    );
    const xLeft = 350;
    const xRight = 830;
    const y = linearScale([1, data.configurations.length], [72, height - 55]);
    addText(svg, "ANALOG RANK", {
      x: xLeft,
      y: 30,
      class: "panel-label",
      "text-anchor": "middle",
    });
    addText(svg, "DIGITAL RANK", {
      x: xRight,
      y: 30,
      class: "panel-label",
      "text-anchor": "middle",
    });
    svg.append(svgElement("line", {
      x1: xLeft, x2: xLeft, y1: 50, y2: height - 35, class: "axis-line",
    }));
    svg.append(svgElement("line", {
      x1: xRight, x2: xRight, y1: 50, y2: height - 35, class: "axis-line",
    }));

    for (const configuration of selected) {
      const left = analogById.get(configuration.id);
      const right = digitalById.get(configuration.id);
      const color = schedulerColors[configuration.index];
      const path = `M${xLeft},${y(left.rank)} L${xRight},${y(right.rank)}`;
      const hit = svgElement("path", {
        d: path,
        fill: "none",
        stroke: "transparent",
        "stroke-width": 16,
        role: "button",
      });
      bindTooltip(
        hit,
        configuration.label,
        `rank ${left.rank} → ${right.rank}`,
        [
          ["Analog regret", `${left.score.toFixed(3)}×`],
          ["Digital regret", `${right.score.toFixed(3)}×`],
        ],
      );
      svg.append(hit);
      svg.append(svgElement("path", {
        d: path,
        class: "rank-line",
        stroke: color,
      }));
      renderMarker(svg, configuration.index, xLeft, y(left.rank), color, 0.75);
      renderMarker(svg, configuration.index, xRight, y(right.rank), color, 0.75);
      addText(svg, `${left.rank}. ${configuration.label}`, {
        x: xLeft - 17,
        y: y(left.rank) + 4,
        class: "rank-label",
        "text-anchor": "end",
        fill: color,
        "font-size": 9,
      });
      addText(svg, `${right.rank}. ${configuration.label}`, {
        x: xRight + 17,
        y: y(right.rank) + 4,
        class: "rank-label",
        fill: color,
        "font-size": 9,
      });
    }
  }

  function standardPoint(result, x, y, value, details = []) {
    const configuration = configurationById.get(result.configuration);
    return {
      x,
      y,
      color: modeColors[result.mode],
      configurationIndex: configuration.index,
      radius: 5 + data.tokens.indexOf(result.tokens) * 0.7,
      title: `${configuration.label} · ${result.mode}`,
      value,
      details: [["Tokens", result.tokens], ...details],
    };
  }

  function renderObjective() {
    const points = selectedResults().map((result) =>
      standardPoint(
        result,
        result.normalizedGraphScore,
        result.normalizedRuntime,
        `${result.normalizedRuntime.toFixed(3)}× runtime`,
        [["Score regret", `${result.normalizedGraphScore.toFixed(2)}×`]],
      ));
    scatterChart("#chart-objective", points, {
      xLabel: "NORMALIZED COMPILER GRAPH SCORE",
      yLabel: "NORMALIZED RUNTIME",
      xFormat: (value) => `${value.toFixed(1)}×`,
      yFormat: (value) => `${value.toFixed(2)}×`,
      referenceX: 1,
      referenceY: 1,
    });
  }

  function renderInstructions() {
    const points = selectedResults().map((result) =>
      standardPoint(
        result,
        result.instructionBillions,
        result.runtimeMs,
        formatRuntime(result.runtimeMs),
        [
          ["Instructions", `${result.instructionBillions.toFixed(3)}B`],
          ["Vector share", formatPercent(result.vectorFraction * 100)],
        ],
      ));
    scatterChart("#chart-instructions", points, {
      xLabel: "RETIRED INSTRUCTIONS · BILLIONS",
      yLabel: "SIMULATED RUNTIME",
      xFormat: (value) => `${value.toFixed(1)}B`,
      yFormat: formatRuntime,
      xZero: true,
      yZero: true,
    });
  }

  function renderOffload() {
    const points = pairedResults().map(({ configuration, tokens, analog, digital }) => {
      const instructionRatio = digital.instructions / analog.instructions;
      const speedup = digital.runtimeNs / analog.runtimeNs;
      return {
        x: instructionRatio,
        y: speedup,
        color: tokenColors[tokens],
        configurationIndex: configuration.index,
        radius: 6,
        title: configuration.label,
        value: `${speedup.toFixed(2)}× analog speedup`,
        details: [
          ["Tokens", tokens],
          ["Instruction ratio", `${instructionRatio.toFixed(2)}×`],
          ["Instructions removed", formatPercent((1 - 1 / instructionRatio) * 100)],
        ],
      };
    });
    scatterChart("#chart-offload", points, {
      xLabel: "DIGITAL ÷ ANALOG INSTRUCTIONS",
      yLabel: "DIGITAL ÷ ANALOG RUNTIME",
      xFormat: (value) => `${value.toFixed(1)}×`,
      yFormat: (value) => `${value.toFixed(1)}×`,
      referenceY: 1,
    });
  }

  function renderTransfer() {
    const points = selectedResults().map((result) =>
      standardPoint(
        result,
        result.transferMegabytes,
        result.runtimeMs,
        formatRuntime(result.runtimeMs),
        [
          ["Transfer", `${result.transferMegabytes.toFixed(2)} MB`],
          ["Network words", formatCompact(result.networkWords)],
        ],
      ));
    scatterChart("#chart-transfer", points, {
      xLabel: "INTER-CORE PAYLOAD · MB",
      yLabel: "SIMULATED RUNTIME",
      xFormat: (value) => `${value.toFixed(0)} MB`,
      yFormat: formatRuntime,
      xZero: true,
      yZero: true,
    });
  }

  function renderPareto() {
    const points = selectedResults().map((result) => {
      const point = standardPoint(
        result,
        result.normalizedTransfer,
        result.normalizedInstructions,
        `${result.normalizedRuntime.toFixed(3)}× runtime`,
        [
          ["Transfer cost", `${result.normalizedTransfer.toFixed(2)}×`],
          ["Compute cost", `${result.normalizedInstructions.toFixed(2)}×`],
        ],
      );
      point.radius = 5 + Math.min(5, (result.normalizedRuntime - 1) * 10);
      return point;
    });
    scatterChart("#chart-pareto", points, {
      width: 1180,
      height: 500,
      xLabel: "NORMALIZED TRANSFER VOLUME · LOWER IS BETTER",
      yLabel: "NORMALIZED INSTRUCTION WORK",
      xFormat: (value) => `${value.toFixed(1)}×`,
      yFormat: (value) => `${value.toFixed(1)}×`,
      referenceX: 1,
      referenceY: 1,
      margin: { top: 28, right: 30, bottom: 58, left: 76 },
    });
  }

  function renderActiveCores() {
    const points = selectedResults().map((result) =>
      standardPoint(
        result,
        result.activeCores,
        result.normalizedRuntime,
        `${result.normalizedRuntime.toFixed(3)}× runtime`,
        [["Active cores", result.activeCores]],
      ));
    scatterChart("#chart-active-cores", points, {
      xLabel: "ACTIVE CORES",
      yLabel: "NORMALIZED RUNTIME",
      xFormat: (value) => value.toFixed(0),
      yFormat: (value) => `${value.toFixed(2)}×`,
      referenceY: 1,
      xDomain: [59.5, 64.5],
      xTicks: [60, 61, 62, 63, 64],
    });
  }

  function renderMaxTile() {
    const points = selectedResults().map((result) =>
      standardPoint(
        result,
        result.maxTileMs,
        result.runtimeMs,
        formatRuntime(result.runtimeMs),
        [
          ["Busiest tile", `${result.maxTileMs.toFixed(2)} ms CPU`],
          ["Stretch", `${result.dependencyStretch.toFixed(1)}×`],
        ],
      ));
    scatterChart("#chart-max-tile", points, {
      xLabel: "MAXIMUM TILE CPU TIME · MS AT 1 GHZ",
      yLabel: "SIMULATED MAKESPAN",
      xFormat: (value) => `${value.toFixed(0)} ms`,
      yFormat: formatRuntime,
      xZero: true,
      yZero: true,
    });
  }

  function renderLoadBalance() {
    const points = selectedResults().map((result) =>
      standardPoint(
        result,
        result.loadEfficiency * 100,
        result.normalizedRuntime,
        `${result.normalizedRuntime.toFixed(3)}× runtime`,
        [
          ["Load efficiency", formatPercent(result.loadEfficiency * 100)],
          ["Tile-cycle CV", result.tileCycleCv.toFixed(2)],
        ],
      ));
    scatterChart("#chart-load-balance", points, {
      xLabel: "LOAD-BALANCE EFFICIENCY",
      yLabel: "NORMALIZED RUNTIME",
      xFormat: (value) => `${value.toFixed(0)}%`,
      yFormat: (value) => `${value.toFixed(2)}×`,
      referenceY: 1,
    });
  }

  function renderStretch() {
    const svg = document.querySelector("#chart-stretch");
    const width = 760;
    const height = 430;
    svg.replaceChildren();
    const configurations = selectedConfigurations();
    if (!configurations.length) {
      emptyChart(svg, width, height);
      return;
    }
    const visible = selectedResults();
    const domain = numericDomain(visible.map((result) => result.dependencyStretch), {
      padding: 0.08,
    });
    const margin = { top: 24, right: 28, bottom: 55, left: 185 };
    const x = linearScale(domain, [margin.left, width - margin.right]);
    const row = linearScale(
      [0, Math.max(1, configurations.length - 1)],
      [margin.top + 8, height - margin.bottom - 8],
    );
    for (const tick of ticks(domain, 5)) {
      const position = x(tick);
      svg.append(svgElement("line", {
        x1: position,
        x2: position,
        y1: margin.top,
        y2: height - margin.bottom,
        class: "grid-line",
      }));
      addText(svg, `${tick.toFixed(0)}×`, {
        x: position,
        y: height - margin.bottom + 23,
        class: "tick-label",
        "text-anchor": "middle",
      });
    }
    configurations.forEach((configuration, index) => {
      const y = row(index);
      addText(svg, configuration.label, {
        x: margin.left - 11,
        y: y + 4,
        class: "tick-label",
        "text-anchor": "end",
        "font-size": configurations.length > 9 ? 8 : 10,
      });
      const points = visible.filter((result) =>
        result.configuration === configuration.id);
      for (const result of points) {
        const offset =
          (data.tokens.indexOf(result.tokens) - 1.5) *
          Math.min(3.2, 22 / Math.max(1, configurations.length));
        renderPoint(svg, {
          x: x(result.dependencyStretch),
          y: y + offset,
          color: modeColors[result.mode],
          configurationIndex: configuration.index,
          radius: 4.2,
          title: `${configuration.label} · ${result.mode}`,
          value: `${result.dependencyStretch.toFixed(1)}× stretch`,
          details: [
            ["Tokens", result.tokens],
            ["Runtime", formatRuntime(result.runtimeMs)],
            ["Max tile", `${result.maxTileMs.toFixed(2)} ms`],
          ],
        });
      }
    });
    addText(svg, "DEPENDENCY STRETCH · MAKESPAN ÷ MAX TILE CPU TIME", {
      x: margin.left + (width - margin.left - margin.right) / 2,
      y: height - 11,
      class: "axis-label",
      "text-anchor": "middle",
    });
  }

  function renderAnalogLifecycle() {
    const svg = document.querySelector("#chart-analog-lifecycle");
    const width = 1180;
    const height = 500;
    svg.replaceChildren();
    const selected = results.filter((result) =>
      result.mode === "analog" && state.configurations.has(result.configuration));
    if (!selected.length) {
      emptyChart(svg, width, height);
      return;
    }
    const summaries = data.tokens.map((tokens) => {
      const group = selected.filter((result) => result.tokens === tokens);
      return {
        tokens,
        inputWords: average(group.map((result) => result.analogInputWords)),
        compute: average(group.map((result) => result.analogCompute)),
        set: average(group.map((result) => result.analogSet)),
      };
    });
    const first = summaries[0];
    const last = summaries[summaries.length - 1];
    const perToken =
      (last.inputWords - first.inputWords) / (last.tokens - first.tokens);
    const fixedWords = Math.max(0, first.inputWords - perToken * first.tokens);
    const panelGap = 84;
    const left = { x: 78, width: 465 };
    const right = { x: left.x + left.width + panelGap, width: 465 };
    const top = 58;
    const bottom = 420;
    const maxWords = Math.max(...summaries.map((item) => item.inputWords));
    const maxOps = Math.max(...summaries.map((item) => item.compute));
    const yWords = linearScale([0, maxWords * 1.08], [bottom, top]);
    const yOps = linearScale([0, maxOps * 1.08], [bottom, top]);

    addText(svg, "ANALOG INPUT WORDS", {
      x: left.x + left.width / 2,
      y: 28,
      class: "panel-label",
      "text-anchor": "middle",
    });
    addText(svg, "ANALOG OPERATION COUNTS", {
      x: right.x + right.width / 2,
      y: 28,
      class: "panel-label",
      "text-anchor": "middle",
    });
    for (const value of ticks([0, maxWords * 1.08], 4)) {
      const y = yWords(value);
      svg.append(svgElement("line", {
        x1: left.x,
        x2: left.x + left.width,
        y1: y,
        y2: y,
        class: "grid-line",
      }));
      addText(svg, `${(value / 1e6).toFixed(0)}M`, {
        x: left.x - 10,
        y: y + 4,
        class: "tick-label",
        "text-anchor": "end",
      });
    }
    for (const value of ticks([0, maxOps * 1.08], 4)) {
      const y = yOps(value);
      svg.append(svgElement("line", {
        x1: right.x,
        x2: right.x + right.width,
        y1: y,
        y2: y,
        class: "grid-line",
      }));
      addText(svg, formatCompact(value), {
        x: right.x - 10,
        y: y + 4,
        class: "tick-label",
        "text-anchor": "end",
      });
    }
    const barWidth = 62;
    summaries.forEach((summary, index) => {
      const center =
        left.x + 55 + (index * (left.width - 110)) / (summaries.length - 1);
      const activationWords = perToken * summary.tokens;
      const fixedTop = yWords(fixedWords);
      const totalTop = yWords(fixedWords + activationWords);
      svg.append(svgElement("rect", {
        x: center - barWidth / 2,
        y: fixedTop,
        width: barWidth,
        height: bottom - fixedTop,
        rx: 4,
        fill: "rgba(77, 166, 255, 0.48)",
      }));
      svg.append(svgElement("rect", {
        x: center - barWidth / 2,
        y: totalTop,
        width: barWidth,
        height: fixedTop - totalTop,
        rx: 4,
        fill: "rgba(78, 214, 161, 0.82)",
      }));
      addText(svg, String(summary.tokens), {
        x: center,
        y: bottom + 25,
        class: "tick-label",
        "text-anchor": "middle",
      });
      const hit = svgElement("rect", {
        x: center - barWidth / 2,
        y: totalTop,
        width: barWidth,
        height: bottom - totalTop,
        fill: "transparent",
        role: "button",
      });
      bindTooltip(
        hit,
        `${summary.tokens} tokens · analog input`,
        `${formatCompact(summary.inputWords)} words`,
        [
          ["Fixed weights", formatCompact(fixedWords)],
          ["Activation input", formatCompact(activationWords)],
        ],
      );
      svg.append(hit);
    });

    const xOps = linearScale(
      [Math.min(...data.tokens), Math.max(...data.tokens)],
      [right.x + 35, right.x + right.width - 25],
    );
    const computePath = summaries.map((summary, index) =>
      `${index ? "L" : "M"}${xOps(summary.tokens)},${yOps(summary.compute)}`,
    ).join(" ");
    const setPath = summaries.map((summary, index) =>
      `${index ? "L" : "M"}${xOps(summary.tokens)},${yOps(summary.set)}`,
    ).join(" ");
    svg.append(svgElement("path", {
      d: computePath,
      class: "series-line",
      stroke: "#4ed6a1",
    }));
    svg.append(svgElement("path", {
      d: setPath,
      class: "series-line",
      stroke: "#4da6ff",
      "stroke-dasharray": "7 5",
    }));
    summaries.forEach((summary) => {
      renderPoint(svg, {
        x: xOps(summary.tokens),
        y: yOps(summary.compute),
        color: "#4ed6a1",
        configurationIndex: 0,
        radius: 6,
        title: `${summary.tokens} tokens · analog MVMs`,
        value: `${formatCompact(summary.compute)} compute calls`,
        details: [["Array setup calls", formatCompact(summary.set)]],
      });
      addText(svg, String(summary.tokens), {
        x: xOps(summary.tokens),
        y: bottom + 25,
        class: "tick-label",
        "text-anchor": "middle",
      });
    });
    addText(svg, "TOKEN COUNT", {
      x: left.x + left.width / 2,
      y: 474,
      class: "axis-label",
      "text-anchor": "middle",
    });
    addText(svg, "TOKEN COUNT", {
      x: right.x + right.width / 2,
      y: 474,
      class: "axis-label",
      "text-anchor": "middle",
    });
    addText(svg, "fixed weight stream", {
      x: left.x + 12,
      y: 452,
      class: "tick-label",
      fill: "#4da6ff",
    });
    addText(svg, "token-dependent activations", {
      x: left.x + 160,
      y: 452,
      class: "tick-label",
      fill: "#4ed6a1",
    });
  }

  const ablationDefinitions = [
    ["Timing-aware · L2", "greedy-l2", "greedy-timing-l2"],
    ["Timing-aware · L3", "greedy-l3", "greedy-timing-l3"],
    ["Timing-aware · B8", "greedy-b8", "greedy-timing-b8"],
    ["Link pressure · greedy", "greedy-b8", "greedy-b8-link-pressure"],
    ["Link pressure · timing", "greedy-timing-b8", "greedy-timing-b8-link-pressure"],
    ["Balanced reductions · greedy", "greedy-b8", "greedy-b8-balanced-reductions"],
    ["Balanced reductions · timing", "greedy-timing-b8", "greedy-timing-b8-balanced-reductions"],
    ["Link + reductions · greedy", "greedy-b8", "greedy-b8-link-pressure-balanced-reductions"],
    ["Link + reductions · timing", "greedy-timing-b8", "greedy-timing-b8-link-pressure-balanced-reductions"],
  ];

  function ablationRows() {
    return ablationDefinitions
      .filter(([, baseline, variant]) =>
        state.configurations.has(baseline) && state.configurations.has(variant))
      .map(([label, baseline, variant]) => ({
        label,
        baseline,
        variant,
        values: Object.fromEntries(data.modes.map((mode) => [
          mode,
          average(data.tokens.map((tokens) =>
            100 * (
              resultFor(tokens, variant, mode).runtimeNs /
              resultFor(tokens, baseline, mode).runtimeNs -
              1
            ))),
        ])),
      }));
  }

  function renderAblations() {
    const svg = document.querySelector("#chart-ablations");
    const width = 1180;
    const height = 610;
    svg.replaceChildren();
    const rows = ablationRows();
    if (!rows.length) {
      emptyChart(
        svg,
        width,
        height,
        "Select complete scheduler pairs to view ablations.",
      );
      return;
    }
    const allValues = rows.flatMap((row) => Object.values(row.values));
    const maximum = Math.max(5, ...allValues.map(Math.abs)) * 1.16;
    const margin = { top: 48, right: 65, bottom: 55, left: 270 };
    const x = linearScale([-maximum, maximum], [margin.left, width - margin.right]);
    const rowHeight = (height - margin.top - margin.bottom) / rows.length;
    for (const tick of ticks([-maximum, maximum], 6)) {
      const position = x(tick);
      svg.append(svgElement("line", {
        x1: position,
        x2: position,
        y1: margin.top,
        y2: height - margin.bottom,
        class: Math.abs(tick) < 0.001 ? "reference-line" : "grid-line",
      }));
      addText(svg, `${tick > 0 ? "+" : ""}${tick.toFixed(0)}%`, {
        x: position,
        y: height - margin.bottom + 23,
        class: "tick-label",
        "text-anchor": "middle",
      });
    }
    rows.forEach((row, index) => {
      const center = margin.top + rowHeight * (index + 0.5);
      addText(svg, row.label, {
        x: margin.left - 13,
        y: center + 4,
        class: "bar-label",
        "text-anchor": "end",
      });
      data.modes.forEach((mode, modeIndex) => {
        const value = row.values[mode];
        const offset = modeIndex === 0 ? -7 : 7;
        const start = x(0);
        const end = x(value);
        const bar = svgElement("rect", {
          x: Math.min(start, end),
          y: center + offset - 5,
          width: Math.max(1, Math.abs(end - start)),
          height: 10,
          rx: 3,
          fill: modeColors[mode],
          opacity: value <= 0 ? 0.86 : 0.56,
          role: "button",
        });
        bindTooltip(
          bar,
          `${row.label} · ${mode}`,
          `${value > 0 ? "+" : ""}${value.toFixed(2)}% runtime`,
          [
            ["Baseline", configurationLabel(row.baseline)],
            ["Variant", configurationLabel(row.variant)],
            ["Interpretation", value < 0 ? "Improvement" : "Regression"],
          ],
        );
        svg.append(bar);
        svg.append(svgElement("circle", {
          cx: end,
          cy: center + offset,
          r: 4,
          fill: modeColors[mode],
        }));
      });
    });
    addText(svg, "FASTER", {
      x: x(-maximum * 0.76),
      y: 25,
      class: "panel-label",
      "text-anchor": "middle",
      fill: "#4ed6a1",
    });
    addText(svg, "SLOWER", {
      x: x(maximum * 0.76),
      y: 25,
      class: "panel-label",
      "text-anchor": "middle",
      fill: "#ff6b78",
    });
    addText(svg, "MEAN PAIRED RUNTIME CHANGE WHEN FEATURE IS ENABLED", {
      x: margin.left + (width - margin.left - margin.right) / 2,
      y: height - 10,
      class: "axis-label",
      "text-anchor": "middle",
    });
  }

  function renderSearchBudget() {
    const svg = document.querySelector("#chart-search-budget");
    const width = 1180;
    const height = 500;
    svg.replaceChildren();
    const variants = [
      ["Transfer L2", "greedy-l2"],
      ["Transfer L3", "greedy-l3"],
      ["Transfer B8", "greedy-b8"],
      ["Timing L2", "greedy-timing-l2"],
      ["Timing L3", "greedy-timing-l3"],
      ["Timing B8", "greedy-timing-b8"],
    ].filter(([, id]) => state.configurations.has(id));
    if (!variants.length) {
      emptyChart(svg, width, height, "Select base L2, L3, or B8 schedulers.");
      return;
    }
    const rows = variants.map(([label, id]) => ({
      label,
      id,
      values: Object.fromEntries(data.modes.map((mode) => [
        mode,
        average(data.tokens.map((tokens) =>
          resultFor(tokens, id, mode).normalizedRuntime)),
      ])),
    }));
    const maximum = Math.max(
      1.05,
      ...rows.flatMap((row) => Object.values(row.values)),
    ) * 1.08;
    const margin = { top: 35, right: 35, bottom: 78, left: 75 };
    const y = linearScale([1, maximum], [height - margin.bottom, margin.top]);
    const slotWidth = (width - margin.left - margin.right) / rows.length;
    for (const tick of ticks([1, maximum], 5)) {
      const position = y(tick);
      svg.append(svgElement("line", {
        x1: margin.left,
        x2: width - margin.right,
        y1: position,
        y2: position,
        class: Math.abs(tick - 1) < 0.001 ? "reference-line" : "grid-line",
      }));
      addText(svg, `${tick.toFixed(2)}×`, {
        x: margin.left - 10,
        y: position + 4,
        class: "tick-label",
        "text-anchor": "end",
      });
    }
    rows.forEach((row, index) => {
      const center = margin.left + slotWidth * (index + 0.5);
      data.modes.forEach((mode, modeIndex) => {
        const value = row.values[mode];
        const barWidth = Math.min(38, slotWidth * 0.28);
        const x = center + (modeIndex === 0 ? -barWidth : 0);
        const bar = svgElement("rect", {
          x,
          y: y(value),
          width: barWidth,
          height: y(1) - y(value),
          rx: 4,
          fill: modeColors[mode],
          opacity: 0.78,
          role: "button",
        });
        bindTooltip(
          bar,
          `${row.label} · ${mode}`,
          `${value.toFixed(3)}× mean regret`,
          [["Mean penalty", formatPercent((value - 1) * 100)]],
        );
        svg.append(bar);
      });
      addText(svg, row.label, {
        x: center,
        y: height - margin.bottom + 26,
        class: "tick-label",
        "text-anchor": "middle",
      });
    });
    addText(svg, "MEAN NORMALIZED RUNTIME · LOWER IS BETTER", {
      x: 16,
      y: margin.top + (height - margin.top - margin.bottom) / 2,
      class: "axis-label",
      "text-anchor": "middle",
      transform:
        `rotate(-90 16 ${margin.top + (height - margin.top - margin.bottom) / 2})`,
    });
  }

  function updateNarrative() {
    const winners = {};
    for (const mode of data.modes) {
      const byToken = data.tokens.map((tokens) =>
        results
          .filter((result) => result.mode === mode && result.tokens === tokens)
          .sort((left, right) => left.runtimeNs - right.runtimeNs)[0]);
      const counts = new Map();
      for (const result of byToken) {
        counts.set(result.configuration, (counts.get(result.configuration) ?? 0) + 1);
      }
      const winner = [...counts.entries()].sort((left, right) => right[1] - left[1])[0];
      winners[mode] = { id: winner[0], wins: winner[1] };
    }
    document.querySelector("#finding-analog").textContent =
      configurationLabel(winners.analog.id);
    document.querySelector("#finding-analog-note").textContent =
      `${winners.analog.wins} of ${data.tokens.length} token counts won`;
    document.querySelector("#finding-digital").textContent =
      configurationLabel(winners.digital.id);
    document.querySelector("#finding-digital-note").textContent =
      `${winners.digital.wins} of ${data.tokens.length} token counts won`;

    const allPairs = [];
    for (const configuration of data.configurations) {
      for (const tokens of data.tokens) {
        allPairs.push(
          resultFor(tokens, configuration.id, "digital").runtimeNs /
          resultFor(tokens, configuration.id, "analog").runtimeNs,
        );
      }
    }
    document.querySelector("#finding-speedup").textContent =
      `${Math.min(...allPairs).toFixed(2)}–${Math.max(...allPairs).toFixed(2)}×`;
    const efficiencies = results.map((result) => result.loadEfficiency * 100);
    document.querySelector("#finding-efficiency").textContent =
      `${Math.min(...efficiencies).toFixed(0)}–${Math.max(...efficiencies).toFixed(0)}%`;

    const randomAnalog = data.tokens.map((tokens) =>
      resultFor(tokens, "random-seed0", "analog"));
    const timingAnalog = data.tokens.map((tokens) =>
      resultFor(tokens, "greedy-timing-l3", "analog"));
    const analogGain = data.tokens.map((_, index) =>
      100 * (1 - timingAnalog[index].runtimeNs / randomAnalog[index].runtimeNs));
    document.querySelector("#takeaway-heatmap").textContent =
      `Greedy timing L3 beats random by ${analogGain[0].toFixed(0)}–` +
      `${analogGain.at(-1).toFixed(0)}% in analog mode, while random wins every digital workload.`;
    document.querySelector("#takeaway-speedup").textContent =
      `Analog is faster in all ${allPairs.length} paired trials, but the benefit varies ` +
      `from ${Math.min(...allPairs).toFixed(2)}× to ${Math.max(...allPairs).toFixed(2)}×.`;
    const analogScaling = data.configurations.map((configuration) =>
      resultFor(32, configuration.id, "analog").runtimeNs /
      (8 * resultFor(4, configuration.id, "analog").runtimeNs));
    const digitalScaling = data.configurations.map((configuration) =>
      resultFor(32, configuration.id, "digital").runtimeNs /
      (8 * resultFor(4, configuration.id, "digital").runtimeNs));
    document.querySelector("#takeaway-scaling").textContent =
      `Digital runtime is nearly linear with tokens (${average(digitalScaling).toFixed(2)}× ` +
      `of ideal); analog shows better amortization (${average(analogScaling).toFixed(2)}×).`;
    document.querySelector("#takeaway-rank").textContent =
      "The analog winner becomes a mid-pack digital placement; random moves to first.";

    const instructionRatios = allPairs.map((_, index) => {
      const configuration = data.configurations[Math.floor(index / data.tokens.length)];
      const tokens = data.tokens[index % data.tokens.length];
      return (
        resultFor(tokens, configuration.id, "digital").instructions /
        resultFor(tokens, configuration.id, "analog").instructions
      );
    });
    document.querySelector("#takeaway-instructions").textContent =
      `Digital lowering retires ${Math.min(...instructionRatios).toFixed(2)}–` +
      `${Math.max(...instructionRatios).toFixed(2)}× more instructions.`;
    document.querySelector("#takeaway-balance").textContent =
      `Existing profiles span ${Math.min(...efficiencies).toFixed(1)}–` +
      `${Math.max(...efficiencies).toFixed(1)}% load efficiency.`;
    const stretches = results.map((result) => result.dependencyStretch);
    document.querySelector("#takeaway-stretch").textContent =
      `Makespan is ${Math.min(...stretches).toFixed(1)}–` +
      `${Math.max(...stretches).toFixed(1)}× the busiest tile's CPU time.`;

    const analogFour = resultFor(4, "random-seed0", "analog");
    const analogThirtyTwo = resultFor(32, "random-seed0", "analog");
    document.querySelector("#takeaway-analog-lifecycle").textContent =
      `${analogFour.analogSet} fixed array setups serve ` +
      `${formatCompact(analogFour.analogCompute)} to ` +
      `${formatCompact(analogThirtyTwo.analogCompute)} MVM executions as tokens scale.`;

    const rows = ablationRows();
    const best = rows
      .flatMap((row) => data.modes.map((mode) => ({
        label: row.label,
        mode,
        value: row.values[mode],
      })))
      .sort((left, right) => left.value - right.value)[0];
    document.querySelector("#takeaway-ablations").textContent =
      `${best.label} produces the strongest mean improvement: ` +
      `${Math.abs(best.value).toFixed(1)}% in ${best.mode} mode.`;
    document.querySelector("#takeaway-search").textContent =
      "Deeper or wider search is not monotonically better; the timing model matters more.";

    document.querySelector("#validity-passed").textContent =
      `${data.summary.passed}/${data.summary.total}`;
    const finite = results.reduce((sum, result) => sum + result.finiteElements, 0);
    const output = results.reduce((sum, result) => sum + result.outputElements, 0);
    document.querySelector("#validity-finite").textContent =
      `${formatCompact(finite)}/${formatCompact(output)}`;
  }

  function schedulerMetadata(configuration) {
    const parts = [configuration.schedule];
    if (configuration.lookahead) parts.push(`L${configuration.lookahead}`);
    if (configuration.beamWidth && configuration.beamWidth > 1) {
      parts.push(`B${configuration.beamWidth}`);
    }
    if (configuration.linkPressure) parts.push("link");
    if (configuration.balancedReductions) parts.push("reductions");
    return parts.join(" · ");
  }

  function previewMarker(index) {
    const preview = document.createElement("span");
    preview.className = "marker-preview";
    const icon = svgElement("svg", {
      viewBox: "-12 -12 24 24",
      "aria-hidden": "true",
    });
    icon.append(svgElement("path", {
      d: markerPaths[index],
      fill: "currentColor",
    }));
    preview.append(icon);
    return preview;
  }

  function buildSchedulerControls() {
    schedulerList.replaceChildren();
    data.configurations.forEach((configuration, index) => {
      const label = document.createElement("label");
      label.className = "scheduler-option";
      label.dataset.configuration = configuration.id;
      const input = document.createElement("input");
      input.type = "checkbox";
      input.checked = state.configurations.has(configuration.id);
      input.addEventListener("change", () => {
        if (input.checked) state.configurations.add(configuration.id);
        else state.configurations.delete(configuration.id);
        syncControls();
        renderAll();
      });
      const copy = document.createElement("span");
      copy.className = "scheduler-copy";
      const title = document.createElement("strong");
      title.textContent = configuration.label;
      const metadata = document.createElement("span");
      metadata.textContent = schedulerMetadata(configuration);
      copy.append(title, metadata);
      label.append(input, previewMarker(index), copy);
      schedulerList.append(label);
    });
    syncControls();
  }

  function syncControls() {
    schedulerList.querySelectorAll(".scheduler-option").forEach((option) => {
      const active = state.configurations.has(option.dataset.configuration);
      option.classList.toggle("active", active);
      option.querySelector("input").checked = active;
    });
    document.querySelector("#selected-count").textContent =
      `${state.configurations.size} / ${data.configurations.length} selected`;
  }

  function setConfigurations(ids) {
    state.configurations = new Set(ids);
    syncControls();
    renderAll();
  }

  function renderAll() {
    hideTooltip();
    renderHeatmap();
    renderSpeedup();
    renderScaling();
    renderRankMigration();
    renderObjective();
    renderInstructions();
    renderOffload();
    renderTransfer();
    renderPareto();
    renderActiveCores();
    renderMaxTile();
    renderLoadBalance();
    renderStretch();
    renderAnalogLifecycle();
    renderAblations();
    renderSearchBudget();
  }

  document.querySelector("#select-all").addEventListener("click", () => {
    setConfigurations(data.configurations.map((configuration) => configuration.id));
  });
  document.querySelector("#select-none").addEventListener("click", () => {
    setConfigurations([]);
  });
  document.querySelector("#select-greedy").addEventListener("click", () => {
    setConfigurations(
      data.configurations
        .filter((configuration) => configuration.id.startsWith("greedy"))
        .map((configuration) => configuration.id),
    );
  });
  document.querySelector("#select-headline").addEventListener("click", () => {
    setConfigurations(["random-seed0", "greedy-timing-l3"]);
  });

  for (const mode of data.modes) {
    const input = document.querySelector(`#mode-${mode}`);
    input.addEventListener("change", () => {
      if (input.checked) {
        state.modes.add(mode);
      } else if (state.modes.size > 1) {
        state.modes.delete(mode);
      } else {
        input.checked = true;
      }
      renderAll();
    });
  }

  document.querySelector("#run-status-text").textContent =
    `${data.summary.passed}/${data.summary.total} deployments · ` +
    `${data.summary.profiled} tile profiles`;
  buildSchedulerControls();
  updateNarrative();
  renderAll();
})();
