# Mittens Trace Movie

This subproject renders one continuous MP4 from a measured Mittens performance
profile. Every frame is addressed by an exact simulation timestamp, and the
complete trace is mapped linearly onto the requested movie duration.

It is not a screen recording. Host rendering speed cannot alter packet order,
event order, or the relationship between movie frames and simulated time.

## Visual contract

The film is a full-frame architectural view, not a recorded dashboard:

- pure white background;
- the physical tile mesh occupies nearly the complete frame;
- square, hard-edged tiles and direct Manhattan links;
- no title card, panels, inspector, browser chrome, or execution timeline;
- one minimal persistent color key below the mesh;
- no packet labels or other text over transfers;
- tile state is encoded directly through color;
- packet movement is represented by unlabeled particles; and
- link heat communicates instantaneous traffic and contention.

The restrained state palette is black for digital work, magenta for analog
work, green for receive DMA, amber/orange for packet movement, boldly hatched
warm gray for waiting, and red for contention or backpressure. Apart from the
minimal key, tile numbers are the only persistent text.

## Fidelity contract

The generated movie uses measured SST timestamps for:

- task execution;
- packet injection and arrival;
- receive DMA;
- analog set, load, compute, store, and move operations;
- tile waits; and
- transmit-backpressure intervals.

The profile does not currently record router-by-router packet coordinates.
Intermediate motion is therefore reconstructed linearly over the declared
deterministic XY route. The injection and arrival endpoints remain measured.
This limitation is recorded in the movie manifest.

Detailed task markers are guest-visible diagnostic instrumentation and can
perturb the simulated execution. The movie is faithful to that instrumented
run, but its duration must not replace the uninstrumented performance result
reported by the corresponding summary-only experiment.

The movie displays one particle for each visible packet header or payload.
When more than 192 packets are simultaneously in flight, all packets continue
to contribute to link heat, while the 192 most informative packet heads are
drawn individually.

## Render a trace movie

```bash
python3 visualizer/movie/render-movie.py \
  /absolute/path/to/trace.json \
  /absolute/path/to/movie.mp4
```

Defaults:

- 1920x1080;
- 30 frames per second;
- 90 seconds;
- H.264 with CRF 17; and
- one continuous linear mapping of the complete simulated interval.

## Configuration

```bash
python3 visualizer/movie/render-movie.py \
  /absolute/path/to/trace.json \
  /absolute/path/to/movie.mp4 \
  --seconds 120 --fps 60 --width 3840 --height 2160 --crf 15
```

The renderer requires Pillow and FFmpeg in the host environment.

## Linear frame mapping

For `N` frames and a trace lasting `D` ticks, frame `i` represents:

```text
simulation_tick(i) = D * i / (N - 1)
```

Frame zero is the beginning of the trace. Frame `N - 1` is the end. There are
no title-card holds, event-dependent pauses, cuts, or nonlinear slowdowns.

The movie manifest records:

- input trace SHA-256;
- output movie SHA-256;
- source profile;
- trace timebase and duration;
- frame count and frame rate;
- resolution and encoding settings;
- linear slowdown factor;
- measured versus reconstructed fields; and
- the exact frame-to-simulation-time equation.

## Tests

```bash
./visualizer/movie/run-tests.sh
```

The test exports the existing visualization fixture, verifies frames at the
start, midpoint, and end, encodes a five-frame MP4, and validates its manifest.
