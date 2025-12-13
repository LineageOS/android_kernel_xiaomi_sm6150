# ZRAM Multi-Compression Setup Guide

## Overview

This kernel includes ZRAM Multi-Compression backported from Linux 6.2. This feature allows using multiple compression algorithms:
- **Primary (LZ4)**: Fast compression for hot/active pages
- **Secondary (ZSTD)**: Better compression ratio for idle/cold pages

## How It Works

1. New pages are compressed with LZ4 (fast, ~2.1:1 ratio)
2. Pages marked as "idle" can be recompressed with ZSTD (~2.8:1 ratio)
3. This saves 25-30% more memory on idle pages
4. Hot pages remain fast with LZ4

## Configuration Requirements

**IMPORTANT**: Secondary algorithms MUST be configured BEFORE Android sets the disksize.

The configuration window is:
```
ZRAM device created → Configure algorithms → Set disksize → ZRAM active
```

Once `disksize` is set, algorithm configuration is locked.

## Implementation Options

### Option 1: Magisk Module (Recommended)

Create a Magisk module with `post-fs-data.sh`:

```bash
#!/system/bin/sh
# /data/adb/modules/zram_multi_comp/post-fs-data.sh

MODDIR=${0%/*}

# Wait for zram device
while [ ! -e /sys/block/zram0 ]; do
    sleep 0.1
done

# Configure secondary algorithm BEFORE disksize is set
# Check if disksize is still 0 (not configured yet)
if [ "$(cat /sys/block/zram0/disksize)" = "0" ]; then
    echo zstd > /sys/block/zram0/recomp_algorithm
    echo "ZRAM: Configured ZSTD as secondary compressor" > /dev/kmsg
fi
```

Module structure:
```
zram_multi_comp/
├── module.prop
├── post-fs-data.sh
└── system/
    └── .placeholder
```

`module.prop`:
```
id=zram_multi_comp
name=ZRAM Multi-Compression Config
version=1.0
versionCode=1
author=YourName
description=Configures ZSTD as secondary ZRAM compressor
```

### Option 2: Device Tree Init Script

Add to your device's `init.target.rc` or create new file in `device/xiaomi/toco/init/`:

```rc
on post-fs-data
    # Configure ZRAM secondary compressor before Android sets disksize
    write /sys/block/zram0/recomp_algorithm zstd
```

**Note**: This must execute BEFORE the standard ZRAM initialization in Android init.

### Option 3: Vendor Init Script

Create `/vendor/etc/init/hw/init.zram_multicomp.rc`:

```rc
service zram_multicomp /vendor/bin/sh -c "echo zstd > /sys/block/zram0/recomp_algorithm"
    class core
    user root
    group root
    oneshot
    disabled

on post-fs-data
    start zram_multicomp
```

## Triggering Recompression

After ZRAM is active, trigger recompression of idle pages:

```bash
# Mark pages idle (pages not accessed in last 120 seconds)
echo 120 > /sys/block/zram0/idle

# Recompress idle pages with secondary algorithm (ZSTD)
echo type=idle > /sys/block/zram0/recompress
```

### Automation Script

Create a script to run periodically (e.g., via cron or when screen off):

```bash
#!/system/bin/sh
# /data/local/tmp/zram_recompress.sh

# Mark pages as idle if not accessed in 2 minutes
echo 120 > /sys/block/zram0/idle

# Recompress idle pages
echo "type=idle" > /sys/block/zram0/recompress

# Log stats
echo "ZRAM recompression completed at $(date)" >> /data/local/tmp/zram.log
```

## Verification Commands

```bash
# Check available algorithms
cat /sys/block/zram0/comp_algorithm

# Check configured recompression algorithm
cat /sys/block/zram0/recomp_algorithm

# View ZRAM statistics
cat /sys/block/zram0/mm_stat

# Check debug info (shows recompressed pages with 'r' flag)
cat /sys/kernel/debug/zram/zram0/block_state | grep 'r'

# Count pages by state
cat /sys/kernel/debug/zram/zram0/block_state | awk '{print $2}' | sort | uniq -c
```

## Expected Benefits

| Metric | Without Multi-Comp | With Multi-Comp |
|--------|-------------------|-----------------|
| Hot page compression | LZ4 (~2.1:1) | LZ4 (~2.1:1) |
| Idle page compression | LZ4 (~2.1:1) | ZSTD (~2.8:1) |
| Memory savings (idle) | Baseline | +25-30% |
| Compression speed | Fast | Fast (hot) / Slower (idle) |
| Decompression speed | Fast | Fast (both) |

### Real-world Impact

- **More apps in memory**: Better compression = more apps cached
- **Fewer app reloads**: Apps stay in memory longer
- **Better multitasking**: Switch between apps without reloading
- **Improved battery**: Less I/O for app reloads

## Sysfs Interface Reference

| File | Description |
|------|-------------|
| `/sys/block/zram0/comp_algorithm` | Primary algorithm (read/write before init) |
| `/sys/block/zram0/recomp_algorithm` | Secondary algorithms (write before init) |
| `/sys/block/zram0/idle` | Mark pages idle (write seconds or "all") |
| `/sys/block/zram0/recompress` | Trigger recompression |
| `/sys/block/zram0/mm_stat` | Memory statistics |

### Recompress Options

```bash
# Recompress idle pages
echo "type=idle" > /sys/block/zram0/recompress

# Recompress huge (incompressible) pages
echo "type=huge" > /sys/block/zram0/recompress

# Recompress huge AND idle pages
echo "type=huge_idle" > /sys/block/zram0/recompress

# Specify algorithm priority (algo=1 for first secondary)
echo "type=idle algo=1" > /sys/block/zram0/recompress

# Limit pages to recompress
echo "type=idle threshold=3000" > /sys/block/zram0/recompress
```

## Troubleshooting

### "Algorithm change not allowed"
- Cause: disksize already set
- Solution: Configure algorithm earlier in boot sequence

### recomp_algorithm shows empty
- Cause: No secondary algorithm configured
- Solution: Write algorithm before disksize is set

### No 'r' flags in block_state
- Cause: Recompression not triggered
- Solution: Run idle marking + recompress commands

## Kernel Config Reference

These options are already enabled in `toco.config`:

```
CONFIG_CRYPTO_ZSTD=y
CONFIG_ZRAM_DEF_COMP_LZ4=y
CONFIG_ZRAM_WRITEBACK=y
CONFIG_ZRAM_MEMORY_TRACKING=y
CONFIG_ZRAM_MULTI_COMP=y
```
