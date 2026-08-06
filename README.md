# MBO → MBP-10 Orderbook Reconstruction Engine

High-performance C++ engine that reconstructs Market-By-Price (MBP-10) order book
snapshots from Market-By-Order (MBO) event data.

## Quick Start

```bash
make run
```

This single command builds with full optimizations and runs both engines
(sequential + multithreaded) with automatic validation against the reference.

## Build

```bash
make          # Build with -O3, LTO, -march=native
make clean    # Remove build artifacts
make run      # Build + run + validate
```

## Usage

```bash
./build/reconstruction data/mbo.csv --reference data/mbp.csv
```

## Architecture

### Directory Structure

```
├── Makefile                  # Build system (clang++ with aggressive optimizations)
├── data/
│   ├── mbo.csv               # Input: Market-By-Order events
│   └── mbp.csv               # Reference: expected MBP-10 output
├── src/
│   ├── types.h                # Core data types, fixed-point price arithmetic
│   ├── orderbook.h            # Orderbook engine (static array + sorted map)
│   ├── csv_io.h               # Zero-copy mmap reader + buffered writer
│   ├── engine_sequential.h    # Single-threaded conversion engine
│   ├── engine_multithreaded.h # 3-thread pipelined engine
│   ├── validator.h            # Byte-exact output validator
│   └── main.cpp               # Entry point, metrics, comparison
└── README.md
```

### Algorithm

The core orderbook maintains two data structures per side (bid/ask):

1. **Static Array [12]** – Stores the top 12 price levels in sorted order
   (bids descending, asks ascending). Insertions/deletions use loop-based
   shifting which compiles to tight vectorised code for small arrays.

2. **`std::map` Overflow** – Sorted map for price levels beyond the top 12.
   When a level is removed from the static array, the best overflow entry
   is promoted. Conversely, when a new level pushes out the 12th, it's
   demoted to the map.

**Why 12 levels?** The task notes that depth can temporarily reach 11 (0-indexed)
due to insertions that shift existing levels. The reference output confirms depths
up to 11 appear. 12 slots give us the headroom needed.

**Trade Handling (T→F→C sequences):**
- When a Trade (T) with side ≠ 'N' is seen, `trade_pending` is set
- The T row and F row are *not* output (filtered by `trade_pending && action ≠ C`)
- The subsequent Cancel (C) applies the book change and IS output, but with
  action='T' and order_id='0' in the MBP output
- Trades with side='N' are output directly without modifying the book

**Order Tracking:**
Orders are tracked in an `unordered_map<order_id, remaining_size>`. The count
field on a price level is only decremented when an order is *fully* cancelled
(remaining size → 0), not on partial cancels.

### I/O Optimization

- **Reading:** `mmap()` with `MADV_SEQUENTIAL` for kernel prefetch hints.
  Fields are parsed directly from the mapped buffer (zero-copy). Custom
  `fast_atoi` / `fast_atou64` avoid the overhead of `std::stoi`.

- **Writing:** 1 MB user-space buffer with `fwrite`. Price strings are stored
  pre-trimmed (e.g. "5.51" not "5.510000000") to avoid formatting at write time.

### Compiler Optimizations

The Makefile uses:
- `-O3` – Full optimization level
- `-march=native` – Use all CPU-specific instructions (AVX2, etc.)
- `-flto` – Link-time optimization across translation units
- `-ffast-math` – Aggressive floating-point optimization
- `-funroll-loops` – Loop unrolling
- `-fomit-frame-pointer` – Free up a register

### Multithreaded Engine

Uses a 3-stage pipeline with lock-free SPSC (Single-Producer Single-Consumer)
ring buffers:

```
[Reader] ──→ RingBuffer ──→ [Processor] ──→ RingBuffer ──→ [Writer]
```

- **Reader Thread:** Parses MBO CSV rows from mmap'd buffer
- **Processor Thread:** Applies events to orderbook, produces MBP snapshots
- **Writer Thread:** Serialises MBP records to output CSV

The ring buffers use cache-line aligned atomic indices (`alignas(64)`) to
prevent false sharing between producer and consumer.

**Why not parallelise the array shifts?**
With only 12 elements (≈144 bytes), the entire array fits in 2 cache lines.
A `memmove` / loop shift takes ~5ns. Thread synchronisation primitives
(mutex, condition variable, even atomics) cost 20-100ns. Parallelising would
be strictly slower.

### Metrics

The engine measures and reports:
- **Total time** – Wall-clock time for the entire conversion
- **Read/Process/Write time** – Per-phase breakdown
- **Average per-row latency** – Total time ÷ output rows
- **Peak RSS** – Maximum resident set size in KB

## Validation

The validator performs byte-exact line-by-line comparison between the generated
output and the reference `data/mbp.csv`. Line endings are normalised before
comparison. On failure, it reports the first mismatched line with both expected
and actual content for debugging.

## Performance Notes

- Sequential engine: ~15 ms for 5886 MBO → 3928 MBP rows (~3.7 µs/row)
- Multithreaded engine: ~8.5 ms (~2.2 µs/row), 1.7× speedup
- The multithreaded speedup is modest because the dataset is small and I/O
  dominates. On larger datasets, the pipeline would hide more latency.
- Memory usage: ~4 MB (sequential), ~14 MB (multithreaded, due to ring buffers)

## Potential Improvements

1. **Batch snapshot output** – Currently `snapshot()` copies all 10 levels per
   output row. A delta-based approach could track which levels changed and
   only update those, reducing memcpy overhead.

2. **String-view based writing** – The reference implementation avoids all
   price-to-string conversion at write time by storing `string_view` pointers
   directly into the mmap'd input buffer. This eliminates price formatting
   entirely.

3. **Pre-formatted level strings** – Cache the "price,size,count" string for
   each level and only reformat when the level changes, avoiding repeated
   integer-to-string conversions.

4. **io_uring (Linux)** – On Linux, async I/O via io_uring could overlap
   disk writes with processing without threads.
