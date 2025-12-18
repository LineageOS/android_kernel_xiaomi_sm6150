#!/bin/bash
set -e

KERNEL_DIR=$(pwd)
OUT_DIR="$KERNEL_DIR/out"
JOBS=56

# Lilium Toolchain (LLVM 22 + PGO + BOLT)
TOOLCHAIN_DIR="/home/miguel/Documentos/android/toolchains"
export PATH="$TOOLCHAIN_DIR/bin:$PATH"

echo "========================================"
echo "  Build Kernel - Xiaomi Toco (LineageOS)"
echo "========================================"

# Verificar clang
echo "[*] Usando Lilium Toolchain:"
clang --version | head -1

# Fix CUDA/clang
if grep -q "grep ' version '" scripts/mkcompile_h; then
    sed -i "s/grep ' version '/grep -m1 ' version '/" scripts/mkcompile_h
    echo "[*] Aplicado fix para CUDA/clang"
fi

# Limpiar (usamos rm en lugar de make mrproper para evitar error de KernelSU hook check)
echo "[*] Limpiando..."
rm -rf "$OUT_DIR"
rm -f .config

# Defconfig (atoll es el SoC correcto para toco/SM6150)
echo "[*] Generando defconfig..."
make O="$OUT_DIR" ARCH=arm64 vendor/atoll-perf_defconfig vendor/toco.config

# Compilar
echo "[*] Compilando con $JOBS hilos (Full LTO)..."
make -j"$JOBS" O="$OUT_DIR" \
    ARCH=arm64 \
    CC=clang \
    LD=ld.lld \
    AR=llvm-ar \
    NM=llvm-nm \
    OBJCOPY=llvm-objcopy \
    OBJDUMP=llvm-objdump \
    STRIP=llvm-strip \
    READELF=llvm-readelf \
    CROSS_COMPILE=aarch64-linux-gnu- \
    CROSS_COMPILE_ARM32=arm-linux-gnueabi-

# Verificar
if [ -f "$OUT_DIR/arch/arm64/boot/Image.gz" ]; then
    echo "[✓] Kernel compilado: $OUT_DIR/arch/arm64/boot/Image.gz"
    ls -lh "$OUT_DIR/arch/arm64/boot/Image.gz"
else
    echo "[✗] Error: No se encontró Image.gz"
    exit 1
fi
