# Kernel Optimization Roadmap for SM6150 (4.14)

This document outlines potential performance optimizations that can be backported from newer kernels to improve battery life, performance, and memory management.

## Current Status

### ✅ Already Implemented
| Feature | Source | Impact |
|---------|--------|--------|
| MGLRU (Multi-Gen LRU) | Kernel 6.1 | Better app retention, reduced kswapd CPU |
| BORE Scheduler | Custom | Improved interactivity |
| CASS (CPU Affinity) | Sultan | Better task placement |
| ADIOS I/O Scheduler | Custom | Adaptive latency learning |
| CPU Input Boost | Sultan | Lower input latency |
| Devfreq Boost | Sultan | DDR bandwidth boost on input |
| PSI + lmkd | Android 10+ | Modern memory pressure detection |
| ZRAM LZ4 | Standard | Fast swap compression |
| Sbalance IRQ | Sultan | IRQ load balancing |

---

## 🏆 TOP PRIORITY - High Impact, Feasible

### 1. Lazy RCU (5-10% Battery Savings)

**Impact:** ⭐⭐⭐⭐⭐ Battery
**Difficulty:** Medium
**Source:** Linux 6.2+

Reduces CPU wakeups when the system is idle by batching RCU callbacks and delaying grace periods.

**Benefits:**
- 5-10% power reduction on idle/lightly-loaded systems
- Developed by Google specifically for Android
- Successfully ported by Kirisakura kernel

**Key Config:**
```
CONFIG_RCU_LAZY=y
CONFIG_RCU_LAZY_DEFAULT_OFF=y  # Enable via sysfs
```

**References:**
- https://www.phoronix.com/news/Lazy-RCU-Likely-For-Linux-6.2
- https://lpc.events/event/16/contributions/1204/attachments/985/1937/

---

### 2. ZRAM Writeback Enhancement

**Impact:** ⭐⭐⭐⭐ RAM/Performance
**Difficulty:** Low-Medium
**Source:** Linux 4.14+ (base), 5.x+ (improvements)

Write idle/incompressible pages to storage instead of keeping them in RAM.

**Benefits:**
- Frees RAM without killing apps
- Better memory utilization with MGLRU
- Reduces memory pressure

**Key Config:**
```
CONFIG_ZRAM_WRITEBACK=y
```

**References:**
- https://docs.kernel.org/admin-guide/blockdev/zram.html

---

### 3. ZRAM Multi-Compression (10-20% More Effective RAM)

**Impact:** ⭐⭐⭐⭐ RAM
**Difficulty:** Medium-High
**Source:** Linux 6.1+

Recompress pages with secondary algorithms for better compression ratios.

**How it works:**
1. Initial compression with fast algorithm (LZ4)
2. Recompress idle pages with slower but better algorithm (ZSTD)
3. 10-20% additional compression = more effective RAM

**Key Config:**
```
CONFIG_ZRAM_MULTI_COMP=y
# Primary: lz4 (fast)
# Secondary: zstd (better ratio)
```

**References:**
- https://source.android.com/docs/core/architecture/kernel/release-notes

---

## 🥈 SECOND PRIORITY - Good Impact

### 4. Maple Tree + Per-VMA Locks (20% Faster App Launch)

**Impact:** ⭐⭐⭐⭐⭐ App Launch Time
**Difficulty:** VERY HIGH
**Source:** Linux 6.1+

Replaces red-black tree for VMAs with a new RCU-safe data structure.

**Benefits:**
- Up to 20% faster app launch times
- Reduced mmap_sem lock contention
- Better multi-threaded performance

**Warning:** This is a massive change affecting core MM code. Very difficult to backport to 4.14.

**Key Changes:**
- New maple_tree data structure
- Per-VMA read-write semaphores
- RCU-safe VMA freeing

**References:**
- https://lwn.net/Articles/845507/
- https://lore.kernel.org/lkml/20230227173632.3292573-1-surenb@google.com/

---

### 5. Cluster-Aware Scheduling (~5% Energy Efficiency)

**Impact:** ⭐⭐⭐⭐ CPU/Battery
**Difficulty:** Medium
**Source:** Linux 6.1+

Migrates tasks to cores sharing L2 cache for better efficiency.

**Benefits:**
- ~5% energy efficiency improvement
- Better cache utilization
- Complements existing CASS implementation

**References:**
- https://source.android.com/docs/core/architecture/kernel/release-notes

---

## 🥉 THIRD PRIORITY - Nice to Have

### 6. Lockless Slab Shrinker

**Impact:** ⭐⭐⭐ Performance
**Difficulty:** Medium
**Source:** Linux 6.x+

Reduces lock contention during memory reclaim operations.

**Benefits:**
- Complements MGLRU
- Better performance under memory pressure
- Reduced latency spikes

---

### 7. F2FS Improvements (If Using F2FS)

**Impact:** ⭐⭐⭐ I/O
**Difficulty:** Low
**Source:** Various

**Improvements available:**
- Better garbage collection
- Atomic writes
- Compression improvements

---

## ❌ NOT RECOMMENDED for Kernel 4.14

| Feature | Reason |
|---------|--------|
| **EEVDF Scheduler** | Completely rewrites the scheduler, impossible to port |
| **io_uring** | Too invasive for 4.14, requires massive changes |
| **sched_ext** | Requires modern BPF infrastructure, not compatible |
| **Full Maple Tree** | Requires too many MM subsystem changes |

---

## Implementation Order Recommendation

```
Phase 1 (Battery Focus):
├── Lazy RCU ────────────────> 5-10% battery savings
└── ZRAM Writeback tuning ───> Better memory efficiency

Phase 2 (RAM Focus):
├── ZRAM Multi-Comp ─────────> 10-20% more effective RAM
└── Works great with MGLRU

Phase 3 (Performance Focus):
├── Cluster-aware patches ───> 5% CPU efficiency
└── Lockless slab shrinker ──> Reduced latency
```

---

## Testing Methodology

When implementing any of these optimizations:

1. **Battery Testing:**
   - Idle drain test (screen off, 8 hours)
   - Light usage test (browsing, social media)
   - Heavy usage test (gaming, video)

2. **Performance Testing:**
   - Antutu/Geekbench scores
   - App launch times (cold start)
   - UI smoothness (jank frames)

3. **Memory Testing:**
   - App retention after heavy usage
   - ZRAM compression ratio
   - OOM kill frequency

4. **Stability Testing:**
   - 24-hour stress test
   - Memory pressure scenarios
   - Thermal throttling behavior

---

## References

- [Android Kernel Release Notes](https://source.android.com/docs/core/architecture/kernel/release-notes)
- [kerneltoast (Sultan) GitHub](https://github.com/kerneltoast)
- [Kirisakura Kernel XDA](https://xdaforums.com/t/kernel-05-09-2024-android-14-0-0-stable-kirisakura_raviantah-2-4-2-for-pixel-7-pro-aka-pantah.4509795/)
- [LWN - Lazy RCU](https://lwn.net/Articles/931920/)
- [LWN - Maple Trees](https://lwn.net/Articles/845507/)
- [ZRAM Documentation](https://docs.kernel.org/admin-guide/blockdev/zram.html)
- [Phoronix - Linux 6.6 vs 6.12](https://www.phoronix.com/review/linux-66-612-lts)

---

## Notes

- Simple LMK (Sultan) is NOT compatible with MEMCG, which is required for MGLRU
- Current setup uses lmkd + PSI (same as Pixel devices with MGLRU)
- BORE scheduler already provides good interactivity improvements
- CASS handles CPU affinity, cluster-aware patches would complement it

---

*Last updated: December 2024*
*Kernel version: 4.14.x*
*Device: Xiaomi Mi Note 10 Lite (toco) - SM6150*


Con recompresión activa:
Impacto mínimo en rendimiento - ZSTD descomprime casi tan rápido como LZ4
Beneficio: más memoria libre por mejor compresión
Costo: CPU al momento de recomprimir (solo cuando se ejecuta)
¿Cuándo afectaría rendimiento?
Escenario	Impacto
Recomprimir mientras usas el teléfono	Puede causar lag momentáneo
Recomprimir con pantalla apagada	Cero impacto perceptible
Leer página recomprimida	~5% más lento que LZ4 (imperceptible)
Resumen:
Ahora mismo: cero impacto (código inactivo)
Con Magisk sin recompress: cero impacto
Con recompress en idle/pantalla apagada: beneficio sin costo perceptible
Es como tener un turbo instalado pero apagado - no consume gasolina extra hasta que lo actives.