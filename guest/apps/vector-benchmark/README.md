# Vector Benchmark

Measures a small animated character workload on a real board using
`HostSurface` and flat polygon raster records. The benchmark compares:

- 48, 72 and 96 body vertices (each eye uses half as many vertices);
- hard edges and a palette-interpolated anti-aliasing fringe;
- two and three HostSurface buffers.

Each combination runs 60 warm-up frames followed by 240 measured frames. The
result line reports average `geometry`, `update`, `wait` and `present` times,
plus effective visible FPS inferred from DirectSurface buffer back-pressure.
The final `fastest` line is the raw FPS winner. The `recommended` line selects
an anti-aliased result within 97% of the fastest FPS, preferring more vertices.

Run on an attached board:

```sh
python3 tools/micropixel --transport usb run guest/apps/vector-benchmark \
    --profile performance
```

The benchmark exits after all twelve combinations. Keep captured hardware
logs out of the repository.

## ESP32-P4 Function EV result

Measured on a 1024 x 600 ESP32-P4 Function EV board running firmware 0.9.4.
Times are per-frame averages in microseconds; FPS is the effective visible
rate inferred from surface buffer back-pressure.

| Vertices | AA | Buffers | Geometry | Update | Wait | Present | FPS |
| ---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 48 | off | 2 | 180 | 10,845 | 38,397 | 156 | 20.12 |
| 48 | on | 2 | 181 | 12,654 | 36,593 | 154 | 20.12 |
| 72 | off | 2 | 253 | 12,497 | 36,735 | 159 | 20.09 |
| 72 | on | 2 | 253 | 14,531 | 34,645 | 154 | 20.12 |
| 96 | off | 2 | 331 | 13,677 | 35,413 | 158 | 20.12 |
| 96 | on | 2 | 331 | 16,195 | 32,915 | 144 | 20.12 |
| 48 | off | 3 | 196 | 11,119 | 38,103 | 159 | 20.12 |
| 48 | on | 3 | 196 | 12,795 | 36,432 | 153 | 20.12 |
| 72 | off | 3 | 259 | 12,450 | 36,711 | 157 | 20.12 |
| 72 | on | 3 | 258 | 14,777 | 34,393 | 153 | 20.12 |
| 96 | off | 3 | 321 | 13,800 | 35,300 | 153 | 20.12 |
| 96 | on | 3 | 319 | 16,486 | 32,812 | 146 | 20.05 |

The raw update-time winner is 48 vertices without AA and two buffers. The
recommended production setting is **96 vertices with AA and two buffers**:
it retained the same measured visible FPS while giving the smoothest tested
outline. A third buffer did not improve throughput on this display path and
only increases memory use and latency.
