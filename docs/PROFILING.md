# Profiling notes

## Setup (Ubuntu)

```bash
sudo apt install linux-tools-common linux-tools-generic
sudo sysctl kernel.perf_event_paranoid=1     # allow user-space sampling
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Workflow

1. `scripts/profile.sh map`  and  `scripts/profile.sh array`
2. Compare `perf-results/stat.map.txt` vs `stat.array.txt`:
   * **instructions / cycles (IPC)** - higher IPC means fewer stalls.
   * **L1-dcache-load-misses, LLC-load-misses** - `std::map` nodes are scattered on
     the heap, so expect markedly more misses than the contiguous tick array.
   * **branch-misses** - tree descent branches are hard to predict.
3. Read `perf-results/report.<impl>.txt`. In the map build, look for time in
   `std::_Rb_tree` lookups/inserts/erases (`_M_get_insert_unique_pos`,
   `_Rb_tree_increment`, ...). In the array build those should disappear and
   the remaining hot spots are the id hash lookups and the match loop.
4. Use that evidence to pick the next optimization (e.g. replacing the
   `std::function` trade callback, or a bitset index over the tick array).

## Recording your findings

Paste your own measured numbers here after running on your machine, e.g.:

| metric | map | array |
|---|---|---|
| IPC | | |
| L1-dcache-load-misses | | |
| LLC-load-misses | | |
| top symbol (% cycles) | | |
