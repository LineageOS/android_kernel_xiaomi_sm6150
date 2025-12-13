# MGLRU (Multi-Generational LRU) Backport Guide for Kernel 4.14

## Overview

Este documento contiene toda la informacion necesaria para portar MGLRU (Multi-Generational LRU) desde kernel 6.1/5.15 a kernel 4.14.

**Autor original de MGLRU:** Yu Zhao (yuzhao@google.com) - Google
**Patch series:** v15 (14 patches)
**Kernel objetivo:** 4.14.x (sm6150/toco)

## Referencias Oficiales

- Documentacion oficial: https://docs.kernel.org/admin-guide/mm/multigen_lru.html
- Patch series v15: https://lore.kernel.org/lkml/20220918080010.2920238-1-yuzhao@google.com/T/
- Patch series v14: https://lore.kernel.org/linux-mm/20220831041731.3836322-1-yuzhao@google.com/T/
- Android Common Kernel backport: https://android.googlesource.com/kernel/common/+/e0f24fb5c654ae62356d3eb7d1fd265f9abb83f2
- Raspberry Pi port attempt: https://github.com/raspberrypi/linux/pull/5106

---

## Lista Completa de Patches (14 en total)

### Patch 01: arch_has_hw_pte_young()
**Proposito:** Detectar si el hardware automaticamente setea el bit "accessed" en PTEs.
**Archivos:**
- `arch/arm64/include/asm/pgtable.h`
- `arch/x86/include/asm/pgtable.h`
- `include/linux/pgtable.h`

**Codigo necesario para ARM64:**
```c
#ifndef arch_has_hw_pte_young
static inline bool arch_has_hw_pte_young(void)
{
    return IS_ENABLED(CONFIG_ARM64_HW_AFDBM);
}
#endif
```

### Patch 02: CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG
**Proposito:** Soporte para clearing de bits en PMD entries (solo x86).
**Archivos:**
- `arch/Kconfig`
- `arch/x86/Kconfig`
- `arch/x86/include/asm/pgtable.h`
- `include/linux/pgtable.h`

**Nota:** Para ARM64 esto NO es necesario, solo aplica a x86.

### Patch 03: Refactor shrink_node()
**Proposito:** Preparar vmscan.c para los cambios de MGLRU.
**Archivos:**
- `mm/vmscan.c`

**Cambios:**
- Extraer preparacion de scan count a funcion separada
- Mejorar legibilidad del codigo

### Patch 04: Revert __update_lru_size() folding
**Proposito:** Restaurar `__update_lru_size()` como funcion separada.
**Archivos:**
- `include/linux/mm_inline.h`

**Nota:** En kernel 4.14 esta funcion puede ya existir separada. Verificar primero.

### Patch 05: Multi-gen LRU groundwork (CRITICO)
**Proposito:** Estructura de datos fundamental y funciones base.
**Archivos modificados (16 total, 418 lineas):**

```
fs/fuse/dev.c
include/linux/mm.h
include/linux/mm_inline.h        (+176 lineas)
include/linux/mmzone.h           (+94 lineas)
include/linux/page-flags-layout.h
include/linux/page-flags.h
include/linux/sched.h
kernel/bounds.c
mm/Kconfig
mm/huge_memory.c
mm/memcontrol.c
mm/memory.c
mm/mm_init.c
mm/mmzone.c
mm/swap.c
mm/vmscan.c                      (+73 lineas)
```

**Estructuras de datos clave:**

```c
/* include/linux/mmzone.h */

#define MIN_NR_GENS     2U
#define MAX_NR_GENS     4U

struct lru_gen_struct {
    /* the aging increments the youngest generation number */
    unsigned long max_seq;
    /* the eviction increments the oldest generation numbers */
    unsigned long min_seq[ANON_AND_FILE];
    /* the birth time of each generation in jiffies */
    unsigned long timestamps[MAX_NR_GENS];
    /* the multi-gen LRU lists, lazily sorted on eviction */
    struct list_head lists[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
    /* the multi-gen LRU sizes, eventually consistent */
    long nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
};

/* Agregar a struct lruvec: */
struct lruvec {
    struct list_head        lists[NR_LRU_LISTS];
    struct zone_reclaim_stat reclaim_stat;
    atomic_long_t           inactive_age;
    unsigned long           refaults;
#ifdef CONFIG_LRU_GEN
    /* evictable pages divided into generations */
    struct lru_gen_struct   lrugen;
#endif
};
```

**Funciones clave en mm_inline.h:**

```c
static inline bool lru_gen_enabled(void)
{
#ifdef CONFIG_LRU_GEN
    return static_branch_likely(&lru_gen_caps[LRU_GEN_CORE]);
#else
    return false;
#endif
}

static inline int lru_gen_from_seq(unsigned long seq)
{
    return seq % MAX_NR_GENS;
}

static inline bool lru_gen_is_active(struct lruvec *lruvec, int gen)
{
    unsigned long max_seq = lruvec->lrugen.max_seq;

    VM_WARN_ON_ONCE(gen >= MAX_NR_GENS);

    return gen == lru_gen_from_seq(max_seq) ||
           gen == lru_gen_from_seq(max_seq - 1);
}

static inline void lru_gen_update_size(struct lruvec *lruvec,
                                        struct folio *folio,
                                        int old_gen, int new_gen)
{
    int type = folio_is_file_lru(folio);
    int zone = folio_zonenum(folio);
    int delta = folio_nr_pages(folio);

    VM_WARN_ON_ONCE(old_gen != -1 && old_gen >= MAX_NR_GENS);
    VM_WARN_ON_ONCE(new_gen != -1 && new_gen >= MAX_NR_GENS);
    VM_WARN_ON_ONCE(old_gen == -1 && new_gen == -1);

    if (old_gen >= 0)
        WRITE_ONCE(lruvec->lrugen.nr_pages[old_gen][type][zone],
                   lruvec->lrugen.nr_pages[old_gen][type][zone] - delta);
    if (new_gen >= 0)
        WRITE_ONCE(lruvec->lrugen.nr_pages[new_gen][type][zone],
                   lruvec->lrugen.nr_pages[new_gen][type][zone] + delta);
}

static inline bool lru_gen_add_folio(struct lruvec *lruvec,
                                      struct folio *folio, bool reclaiming)
{
    unsigned long seq;
    unsigned long flags;
    int gen = folio_lru_gen(folio);
    int type = folio_is_file_lru(folio);
    int zone = folio_zonenum(folio);
    struct lru_gen_struct *lrugen = &lruvec->lrugen;

    VM_WARN_ON_ONCE_FOLIO(gen != -1, folio);

    if (folio_test_unevictable(folio))
        return false;

    /* New pages go to youngest generation */
    seq = lrugen->max_seq;
    gen = lru_gen_from_seq(seq);
    flags = (gen + 1UL) << LRU_GEN_PGOFF;

    /* See comment in folio_lru_gen() */
    set_mask_bits(&folio->flags, LRU_GEN_MASK, flags);

    lru_gen_update_size(lruvec, folio, -1, gen);

    if (reclaiming)
        list_add_tail(&folio->lru, &lrugen->lists[gen][type][zone]);
    else
        list_add(&folio->lru, &lrugen->lists[gen][type][zone]);

    return true;
}

static inline bool lru_gen_del_folio(struct lruvec *lruvec,
                                      struct folio *folio, bool reclaiming)
{
    unsigned long flags;
    int gen = folio_lru_gen(folio);

    if (gen < 0)
        return false;

    VM_WARN_ON_ONCE_FOLIO(gen >= MAX_NR_GENS, folio);

    /* For shrink_page_list() */
    if (reclaiming)
        list_del(&folio->lru);
    else
        list_del_init(&folio->lru);

    /* Clear generation bits */
    flags = READ_ONCE(folio->flags);
    flags &= ~LRU_GEN_MASK;
    WRITE_ONCE(folio->flags, flags);

    lru_gen_update_size(lruvec, folio, gen, -1);

    return true;
}
```

### Patch 06: Minimal implementation (CRITICO)
**Proposito:** Implementacion minima de aging y eviction.
**Archivos:**
- `mm/vmscan.c` (cambios grandes)

**Funciones principales:**
- `lru_gen_age_node()` - Aging de generaciones
- `evict_folios()` - Eviccion de paginas
- `try_to_shrink_lruvec()` - Integra con shrink_node()

### Patch 07: Exploit locality in rmap
**Proposito:** Optimizar usando localidad espacial.
**Archivos:**
- `include/linux/mmzone.h`
- `mm/rmap.c`
- `mm/vmscan.c`

**Funcion clave:**
```c
void lru_gen_look_around(struct page_vma_mapped_walk *pvmw)
{
    /* Escanear PTEs adyacentes cuando se encuentra uno young */
}
```

### Patch 08: Support page table walks
**Proposito:** Caminar page tables para mejor deteccion.
**Archivos:**
- `include/linux/memcontrol.h`
- `include/linux/mm_types.h`
- `include/linux/mmzone.h`
- `mm/memcontrol.c`
- `mm/vmscan.c`

**Estructuras adicionales:**
```c
struct lru_gen_mm_list {
    struct list_head fifo;
    spinlock_t lock;
};

struct lru_gen_mm_state {
    unsigned long seq;
    struct list_head *head;
    struct list_head *tail;
    unsigned long *filters[NR_BLOOM_FILTERS];
};
```

### Patch 09: Optimize multiple memcgs
**Proposito:** Mejorar rendimiento con multiples memcgs.
**Archivos:**
- `mm/vmscan.c`

### Patch 10: Kill switch (IMPORTANTE)
**Proposito:** Control en runtime via sysfs.
**Archivos:**
- `include/linux/cgroup.h`
- `kernel/cgroup/cgroup.c`
- `mm/vmscan.c`

**Sysfs interface:**
```
/sys/kernel/mm/lru_gen/enabled
```

**Valores:**
- 0x0001: Core MGLRU
- 0x0002: Page table walks
- 0x0004: Non-leaf PMD clearing

### Patch 11: Thrashing prevention
**Proposito:** Prevenir thrashing protegiendo working set.
**Archivos:**
- `mm/vmscan.c`

**Sysfs interface:**
```
/sys/kernel/mm/lru_gen/min_ttl_ms
```

### Patch 12: Debugfs interface
**Proposito:** Interface para debugging y proactive reclaim.
**Archivos:**
- `mm/vmscan.c`

**Debugfs files:**
```
/sys/kernel/debug/lru_gen
/sys/kernel/debug/lru_gen_full
```

### Patch 13-14: Documentacion
**Archivos:**
- `Documentation/admin-guide/mm/multigen_lru.rst`

---

## Kconfig Necesario

```kconfig
# mm/Kconfig

config LRU_GEN
    bool "Multi-Gen LRU"
    depends on MMU
    # Dependencia que puede necesitar ajuste para 4.14:
    # depends on !MAXSMP && (64BIT || !SPARSEMEM || SPARSEMEM_VMEMMAP)
    help
      A high performance LRU implementation to overcommit memory.

config LRU_GEN_ENABLED
    bool "Enable by default"
    depends on LRU_GEN
    help
      Turn on MGLRU by default.

config LRU_GEN_STATS
    bool "Full stats for debugging"
    depends on LRU_GEN
    help
      Keep full stats for debugging (uses more memory).

config NR_LRU_GENS
    int "Max number of generations"
    depends on LRU_GEN
    range 4 31
    default 7

config TIERS_PER_GEN
    int "Number of tiers per generation"
    depends on LRU_GEN
    range 2 5
    default 4
```

---

## Desafios Especificos para Kernel 4.14

### 1. API de Folio vs Page
Kernel 4.14 usa `struct page`, kernels modernos usan `struct folio`.

**Solucion:** Crear macros de compatibilidad:
```c
#ifndef CONFIG_HAVE_FOLIO
#define folio page
#define folio_nr_pages(f) compound_nr(f)
#define folio_test_unevictable(f) PageUnevictable(f)
#define folio_is_file_lru(f) page_is_file_cache(f)
#define folio_zonenum(f) page_zonenum(f)
#define folio_lru_gen(f) page_lru_gen(f)
#endif
```

### 2. Static Keys/Branches
Verificar que `static_branch_likely()` existe en 4.14.

### 3. mm_struct Changes
La estructura `mm_struct` ha cambiado. Verificar campos disponibles.

### 4. memcontrol API
La API de memcg ha evolucionado. Puede necesitar ajustes.

### 5. Page Flags
Kernel 4.14 tiene menos bits disponibles en page flags. Verificar:
```c
BITS_PER_LONG - ZONES_WIDTH - SECTIONS_WIDTH - NODES_WIDTH - etc
```

---

## Orden de Aplicacion de Patches

1. **Fase 1 - Infraestructura:**
   - Patch 01 (arch_has_hw_pte_young)
   - Patch 03 (refactor shrink_node)
   - Patch 04 (revert __update_lru_size)

2. **Fase 2 - Core:**
   - Patch 05 (groundwork) - MAS IMPORTANTE
   - Patch 06 (minimal implementation) - MAS IMPORTANTE

3. **Fase 3 - Optimizaciones:**
   - Patch 07 (rmap locality)
   - Patch 08 (page table walks)
   - Patch 09 (memcg optimization)

4. **Fase 4 - Control:**
   - Patch 10 (kill switch)
   - Patch 11 (thrashing prevention)
   - Patch 12 (debugfs)

---

## Archivos Clave a Modificar en Kernel 4.14

```
include/linux/mmzone.h          - struct lru_gen_struct, modificar lruvec
include/linux/mm_inline.h       - Funciones lru_gen_*
include/linux/mm.h              - Macros LRU_GEN_PGOFF
include/linux/page-flags.h      - LRU_GEN_MASK
include/linux/page-flags-layout.h - Bits para generaciones
mm/vmscan.c                     - Logica principal de aging/eviction
mm/Kconfig                      - Opciones de configuracion
mm/swap.c                       - Integracion con page cache
mm/memory.c                     - Fault tracking
mm/memcontrol.c                 - Hooks de memcg
arch/arm64/include/asm/pgtable.h - arch_has_hw_pte_young()
```

---

## Testing

### Verificar compilacion:
```bash
make ARCH=arm64 defconfig
echo "CONFIG_LRU_GEN=y" >> .config
echo "CONFIG_LRU_GEN_ENABLED=y" >> .config
make ARCH=arm64 -j$(nproc)
```

### Verificar en runtime:
```bash
# Verificar soporte
cat /proc/config.gz | gunzip | grep CONFIG_LRU_GEN

# Verificar estado
cat /sys/kernel/mm/lru_gen/enabled

# Habilitar
echo y > /sys/kernel/mm/lru_gen/enabled

# Verificar stats
cat /sys/kernel/debug/lru_gen
```

---

## Beneficios Esperados

Segun Google (ChromeOS y Android):
- **40%** reduccion en uso de CPU de kswapd
- **85%** menos low-memory kills (percentil 75)
- **18%** menos OOM kills en Android
- **16%** reduccion en code starts
- **96%** menos tab discards en Chrome OS

---

## Referencias de Kernels con MGLRU Portado

1. **Kernel 4.19 (OnePlus 6/6T):**
   https://github.com/shinichi-c/Dynamic_kernel_4.19_oneplus_sdm845

2. **Raspberry Pi (5.15):**
   https://github.com/raspberrypi/linux/pull/5106

3. **Android Common Kernel (5.10+):**
   https://android.googlesource.com/kernel/common/

---

## Notas Adicionales

- MGLRU es incompatible con UKSM (usan diferentes estrategias de LRU)
- Si se porta MGLRU, considerar remover UKSM completamente
- El port requiere testing extensivo antes de uso en produccion
- Comenzar con patches 01, 05, 06 y 10 para tener funcionalidad basica

---

---

## URLs de Patches v15 (Descarga Directa)

Todos los patches estan en lore.kernel.org. Agregar `/raw` para obtener el mbox.

### Serie completa v15:
```
Base URL: https://lore.kernel.org/lkml/20220918080010.2920238-{N}-yuzhao@google.com/

Patch 01/14 - arch_has_hw_pte_young:
https://lore.kernel.org/lkml/20220918080010.2920238-2-yuzhao@google.com/raw

Patch 02/14 - CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG:
https://lore.kernel.org/lkml/20220918080010.2920238-3-yuzhao@google.com/raw

Patch 03/14 - refactor shrink_node:
https://lore.kernel.org/lkml/20220918080010.2920238-4-yuzhao@google.com/raw

Patch 04/14 - revert __update_lru_size:
https://lore.kernel.org/lkml/20220918080010.2920238-5-yuzhao@google.com/raw

Patch 05/14 - groundwork (CRITICO):
https://lore.kernel.org/lkml/20220918080010.2920238-6-yuzhao@google.com/raw

Patch 06/14 - minimal implementation (CRITICO):
https://lore.kernel.org/lkml/20220918080010.2920238-7-yuzhao@google.com/raw

Patch 07/14 - exploit locality in rmap:
https://lore.kernel.org/lkml/20220918080010.2920238-8-yuzhao@google.com/raw

Patch 08/14 - support page table walks:
https://lore.kernel.org/lkml/20220918080010.2920238-9-yuzhao@google.com/raw

Patch 09/14 - optimize multiple memcgs:
https://lore.kernel.org/lkml/20220918080010.2920238-10-yuzhao@google.com/raw

Patch 10/14 - kill switch:
https://lore.kernel.org/lkml/20220918080010.2920238-11-yuzhao@google.com/raw

Patch 11/14 - thrashing prevention:
https://lore.kernel.org/lkml/20220918080010.2920238-12-yuzhao@google.com/raw

Patch 12/14 - debugfs interface:
https://lore.kernel.org/lkml/20220918080010.2920238-13-yuzhao@google.com/raw

Patch 13/14 - admin guide:
https://lore.kernel.org/lkml/20220918080010.2920238-14-yuzhao@google.com/raw

Patch 14/14 - design doc:
https://lore.kernel.org/lkml/20220918080010.2920238-15-yuzhao@google.com/raw
```

### Descargar todos los patches:
```bash
mkdir -p mglru_patches && cd mglru_patches
for i in $(seq 2 15); do
    wget -O "patch_$(printf '%02d' $((i-1))).mbox" \
        "https://lore.kernel.org/lkml/20220918080010.2920238-${i}-yuzhao@google.com/raw"
done
```

### Cover letter (descripcion general):
```
https://lore.kernel.org/lkml/20220918080010.2920238-1-yuzhao@google.com/
```

---

## Kernel 4.19 con MGLRU ya portado (referencia)

Este kernel ya tiene MGLRU funcionando en 4.19, puede servir como referencia:

**Repository:** https://github.com/shinichi-c/Dynamic_kernel_4.19_oneplus_sdm845

Para ver los commits de MGLRU en ese repo:
```bash
git clone https://github.com/shinichi-c/Dynamic_kernel_4.19_oneplus_sdm845
cd Dynamic_kernel_4.19_oneplus_sdm845
git log --oneline --all --grep="LRU" --grep="lru_gen"
```

---

## Estrategia de Port Recomendada

### Paso 1: Preparar el entorno
```bash
cd /path/to/kernel
git checkout -b mglru-port
```

### Paso 2: Descargar patches
```bash
mkdir mglru_patches
# Descargar los 14 patches usando el script de arriba
```

### Paso 3: Intentar aplicar patches secuencialmente
```bash
# Empezar con patch 05 (groundwork) que es el mas importante
git apply --check mglru_patches/patch_05.mbox
# Si falla, aplicar manualmente las partes que funcionan
```

### Paso 4: Resolver conflictos
Los principales conflictos seran en:
- `mm/vmscan.c` - ha cambiado mucho entre 4.14 y 5.x
- `include/linux/mm_inline.h` - API de folio vs page
- `include/linux/mmzone.h` - estructuras diferentes

### Paso 5: Crear capa de compatibilidad
```c
/* include/linux/mglru_compat.h */

#ifndef _LINUX_MGLRU_COMPAT_H
#define _LINUX_MGLRU_COMPAT_H

/* Folio compatibility for kernel < 5.16 */
#define folio page
#define folio_test_lru(f) PageLRU(f)
#define folio_test_unevictable(f) PageUnevictable(f)
#define folio_is_file_lru(f) page_is_file_cache(f)
#define folio_nr_pages(f) compound_nr(f)
#define folio_zonenum(f) page_zonenum(f)
#define folio_memcg(f) page_memcg(f)
#define folio_nid(f) page_to_nid(f)

/* Functions */
static inline struct lruvec *folio_lruvec(struct folio *folio)
{
    return mem_cgroup_page_lruvec((struct page *)folio,
                                  page_pgdat((struct page *)folio));
}

#endif /* _LINUX_MGLRU_COMPAT_H */
```

### Paso 6: Testing incremental
```bash
# Compilar despues de cada patch
make ARCH=arm64 -j$(nproc) 2>&1 | tee build.log
# Revisar errores y corregir
```

---

## Checklist de Port

- [ ] Patch 01: arch_has_hw_pte_young() para ARM64
- [ ] Patch 03: Refactor shrink_node()
- [ ] Patch 04: Revert __update_lru_size() (verificar si necesario)
- [ ] Patch 05: Groundwork - estructuras base
- [ ] Patch 06: Minimal implementation - aging/eviction
- [ ] Patch 10: Kill switch - control sysfs
- [ ] Kconfig: Agregar opciones CONFIG_LRU_GEN*
- [ ] Compatibilidad folio/page
- [ ] Testing: Boot exitoso
- [ ] Testing: /sys/kernel/mm/lru_gen/enabled funciona
- [ ] Testing: Stress test de memoria
- [ ] Opcional: Patches 07-09 (optimizaciones)
- [ ] Opcional: Patches 11-12 (thrashing prevention, debugfs)

---

*Documento creado para port de MGLRU a kernel xiaomi sm6150 (toco) 4.14*
*Fecha: 2025*
