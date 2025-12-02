#!/bin/bash
set -e

KERNEL_DIR=$(pwd)
OUT_DIR="$KERNEL_DIR/out"
JOBS=36

echo "========================================"
echo "  Build Kernel - Xiaomi Toco (LineageOS)"
echo "========================================"

# Verificar clang
echo "[*] Usando clang del sistema:"
clang --version | head -1

# Fix CUDA/clang
if grep -q "grep ' version '" scripts/mkcompile_h; then
    sed -i "s/grep ' version '/grep -m1 ' version '/" scripts/mkcompile_h
    echo "[*] Aplicado fix para CUDA/clang"
fi

# Limpiar
echo "[*] Limpiando..."
rm -rf "$OUT_DIR"
make mrproper

# Defconfig
echo "[*] Generando defconfig..."
make O="$OUT_DIR" ARCH=arm64 vendor/sdmsteppe-perf_defconfig vendor/toco.config

# Compilar
echo "[*] Compilando con $JOBS hilos..."
make -j"$JOBS" O="$OUT_DIR" \
    ARCH=arm64 \
    CC=clang \
    LD=ld.lld \
    AR=llvm-ar \
    NM=llvm-nm \
    OBJCOPY=llvm-objcopy \
    OBJDUMP=llvm-objdump \
    STRIP=llvm-strip \
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