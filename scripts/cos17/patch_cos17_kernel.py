#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_cos17_kernel.py

Встраивает OPPO hybridswap (integrated zram flavour) в MoonWake 4.19.325
для Xiaomi ruby (Redmi Note 12 Pro 5G, MT6877), чтобы userspace ColorOS 17
мог поднять nandswap вместо `err=1004 (ERR_UNSUPPORTED)`.

Донор файлов zram/hybridswap: Realme 9 Pro+ / Narzo 50 Pro 5G (AndroidT,
тот же MT6877, Oplus 4.19.191) —
  realme-kernel-opensource/realme_9pro-plus_narzo50pro-5g-AndroidT-kernel-source
  @ a0105615b00a912a0e77045b6e78574051c97146
Скачивает их apply_cos17_hybridswap.sh; здесь только патчи остального дерева.

Скрипт идемпотентный: повторный запуск ничего не ломает.
Все правки сделаны по анкорам (точным фрагментам текста), а не по номерам
строк, поэтому переживают небольшие расхождения между 4.19.191 и 4.19.325.

Использование:
    python3 patch_cos17_kernel.py <KERNEL_ROOT> [--check]

    --check   только проверить, что все анкоры найдены; ничего не писать
"""

import os
import sys

RED = "\033[31m"
GRN = "\033[32m"
YEL = "\033[33m"
CYN = "\033[36m"
RST = "\033[0m"


def col(code, s):
    return code + s + RST


CONFIG_FRAGMENT = """\
# ============================================================
#  COS17 (ColorOS 17) kernel feature set for ruby / MoonWake 4.19.325
#
#  ВАЖНО: этот фрагмент должен применяться ПОСЛЕДНИМ, иначе
#  vendor/slmk.config (CONFIG_MEMCG=n) снова выключит memcg.
# ============================================================

# --- memcg: обязателен для hybridswap (учёт по приложениям, swapd) ---
CONFIG_CGROUPS=y
CONFIG_MEMCG=y
CONFIG_MEMCG_SWAP=y
CONFIG_MEMCG_SWAP_ENABLED=y
CONFIG_MEMCG_KMEM=y
CONFIG_PSI=y

# --- swap / zram ---
CONFIG_SWAP=y
CONFIG_ZSMALLOC=y
CONFIG_ZRAM=y
# CONFIG_ZRAM_WRITEBACK is not set
CONFIG_CRYPTO_LZO=y
CONFIG_CRYPTO_LZ4=y
CONFIG_CRYPTO_LZ4HC=y
CONFIG_CRYPTO_ZSTD=y

# --- OPPO hybridswap (integrated zram flavour) ---
CONFIG_HYBRIDSWAP=y
CONFIG_HYBRIDSWAP_SWAPD=y
CONFIG_HYBRIDSWAP_CORE=y
# CONFIG_HYBRIDSWAP_ASYNC_COMPRESS is not set

# --- Android vendor hooks ---
# КРИТИЧНО: hybridswap регистрирует хуки через register_trace_android_vh_*.
# Без real tracepoints они превращаются в заглушки, register_trace_*
# возвращает -ENOSYS, hybridswap_pre_init() падает -> zram_init()
# отменяется целиком (пропадает zram0).
CONFIG_ANDROID=y
CONFIG_ANDROID_VENDOR_HOOKS=y
# TRACING/TRACEPOINTS — скрытые символы: напрямую их не задать, их
# включает видимый трассировщик. FUNCTION_TRACER уже есть в рабочем
# ядре (в dmesg: tracer_init_tracefs, "Tracing variant of Tasks RCU
# enabled"), фиксируем явно, чтобы ANDROID_VENDOR_HOOKS не выключился
# молча из-за `depends on TRACEPOINTS`.
CONFIG_FUNCTION_TRACER=y

# --- OPPO zram extras: на первом bring-up держим выключенными ---
# CONFIG_OPLUS_ZRAM_OPT is not set
# CONFIG_CONT_PTE_HUGEPAGE is not set
# CONFIG_CONT_PTE_HUGEPAGE_64K_ZRAM is not set

# --- AF_VSOCK: убирает crash-loop `traced` (vsock://-1:2010) ---
CONFIG_VSOCKETS=y
CONFIG_VSOCKETS_DIAG=y
"""


class Patcher:
    def __init__(self, root, check=False):
        self.root = root
        self.check = check
        self.errors = []
        self.applied = []
        self.skipped = []

    # ---------- low level ----------

    def _path(self, rel):
        return os.path.join(self.root, rel)

    def _read(self, rel):
        with open(self._path(rel), "r", encoding="utf-8",
                  errors="surrogateescape", newline="") as f:
            return f.read()

    def _write(self, rel, text):
        with open(self._path(rel), "w", encoding="utf-8",
                  errors="surrogateescape", newline="") as f:
            f.write(text)

    def fail(self, msg):
        self.errors.append(msg)
        print(col(RED, "  FAIL ") + msg)

    def ok(self, msg):
        self.applied.append(msg)
        print(col(GRN, "  ok   ") + msg)

    def skip(self, msg, suffix=" (already applied)"):
        self.skipped.append(msg)
        print(col(YEL, "  skip ") + msg + col(YEL, suffix))

    # ---------- text patching ----------

    def patch(self, rel, old, new, label, marker=None, count=1):
        """Заменить old -> new. marker — подстрока new для проверки идемпотентности."""
        marker = marker if marker is not None else new
        try:
            s = self._read(rel)
        except FileNotFoundError:
            if self.check:
                # Новый файл (vh_vmscan.h) в --check ещё не скачан.
                self.skip("%s: %s (new file, will be downloaded)" % (rel, label), "")
            else:
                self.fail("%s: file not found" % rel)
            return

        crlf = "\r\n" in s

        def fix(x):
            if crlf and "\r\n" not in x:
                return x.replace("\n", "\r\n")
            return x

        old_f, new_f, marker_f = fix(old), fix(new), fix(marker)

        if marker_f in s:
            self.skip("%s: %s" % (rel, label))
            return

        n = s.count(old_f)
        if n != count:
            self.fail("%s: anchor '%s' matched %d time(s), expected %d"
                      % (rel, label, n, count))
            return

        if not self.check:
            self._write(rel, s.replace(old_f, new_f, count))
        self.ok("%s: %s" % (rel, label))

    def ensure_include(self, rel, after, include, label=None):
        """Добавить #include после строки `after` (если его ещё нет)."""
        label = label or include
        try:
            s = self._read(rel)
        except FileNotFoundError:
            if self.check:
                self.skip("%s: %s (new file, will be downloaded)" % (rel, label), "")
            else:
                self.fail("%s: file not found" % rel)
            return
        if include in s:
            self.skip("%s: %s" % (rel, label))
            return
        crlf = "\r\n" in s
        a = after if not crlf else after.replace("\n", "\r\n")
        inc = include if not crlf else include.replace("\n", "\r\n")
        if not a.endswith("\r\n" if crlf else "\n"):
            a += "\r\n" if crlf else "\n"
        if s.count(a) != 1:
            self.fail("%s: include anchor matched %d time(s), expected 1"
                      % (rel, s.count(a)))
            return
        if not self.check:
            self._write(rel, s.replace(a, a + inc + ("\r\n" if crlf else "\n"), 1))
        self.ok("%s: %s" % (rel, label))

    def write_if_missing(self, rel, content):
        p = self._path(rel)
        if os.path.exists(p):
            self.skip("%s: config fragment (уже существует)" % rel)
            return
        if not self.check:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(content)
        self.ok("%s: config fragment" % rel)

    # ---------- checks ----------

    def sanity(self):
        mk = self._path("Makefile")
        if not os.path.isfile(mk):
            self.fail("Makefile not found in %s — это не дерево ядра?" % self.root)
            return False
        try:
            head = "".join(open(mk, "r", encoding="utf-8",
                                errors="surrogateescape").readlines()[:6])
        except OSError:
            return False
        if "VERSION = 4" not in head:
            self.fail("ожидалось ядро 4.x, а в Makefile: %r" % head.replace("\n", " | "))
            return False
        return True


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv[1:]
    root = os.path.abspath(args[0] if args else ".")
    p = Patcher(root, check=check)

    print(col(CYN, "== patch_cos17_kernel.py =="))
    print("   root : %s" % root)
    print("   mode : %s" % ("check only" if check else "write"))
    print()

    if not p.sanity():
        return 1

    # ---------------------------------------------------------------
    # 1. vendor hooks: точка генерации tracepoints
    # ---------------------------------------------------------------
    print(col(CYN, "[1/7] drivers/android/vendor_hooks.c"))
    # gfp.h нужен для gfp_t в DECLARE_HOOK(android_vh_alloc_pages_slowpath)
    p.patch(
        "drivers/android/vendor_hooks.c",
        "#include <trace/hooks/syscall_check.h>\n",
        "#include <trace/hooks/syscall_check.h>\n"
        "#include <linux/gfp.h>\n"
        "#include <trace/hooks/vh_vmscan.h>\n",
        "include vh_vmscan.h (CREATE_TRACE_POINTS)",
        marker="#include <trace/hooks/vh_vmscan.h>",
    )

    # ---------------------------------------------------------------
    # 2. vh_vmscan.h: forward-декларации, чтобы заголовок был самодостаточным
    # ---------------------------------------------------------------
    print(col(CYN, "[2/7] include/trace/hooks/vh_vmscan.h"))
    p.patch(
        "include/trace/hooks/vh_vmscan.h",
        "struct cgroup_subsys;\n",
        "struct cgroup_subsys;\nstruct mem_cgroup;\nstruct seq_file;\nstruct zone;\n",
        "forward declarations",
        marker="struct mem_cgroup;\nstruct seq_file;\nstruct zone;",
    )

    # ---------------------------------------------------------------
    # 2b. hybridmain.c: страховка на этапе сборки
    #     Если vendor hooks выключены, trace_android_vh_* — пустые inline,
    #     а register_trace_* возвращает -ENOSYS. Тогда hybridswap_pre_init()
    #     падает, zram_init() уходит в out_error и /dev/zram0 не создаётся.
    #     Лучше упасть сборкой с внятным сообщением, чем получить телефон
    #     без swap.
    # ---------------------------------------------------------------
    print(col(CYN, "[2b/7] hybridmain.c: vendor hooks guard"))
    p.patch(
        "drivers/block/zram/hybridswap/hybridmain.c",
        "#include <trace/hooks/vh_vmscan.h>\n",
        "#include <trace/hooks/vh_vmscan.h>\n"
        "\n"
        "#if !defined(CONFIG_ANDROID_VENDOR_HOOKS) || !defined(CONFIG_TRACEPOINTS)\n"
        "#error \"COS17: hybridswap needs CONFIG_ANDROID_VENDOR_HOOKS=y (and TRACEPOINTS)\"\n"
        "#endif\n",
        "vendor hooks availability guard",
        marker='#error "COS17: hybridswap needs',
    )

    # ---------------------------------------------------------------
    # 2c. hybridswap.c: у донора пропущен тип возврата у meminfo_show
    #     (возвращает ssize_t `size`, используется как ssize_t). Свежий
    #     clang с -Werror ловит это как implicit-int -> добавим тип.
    # ---------------------------------------------------------------
    print(col(CYN, "[2c/7] hybridswap.c: meminfo_show return type"))
    p.patch(
        "drivers/block/zram/hybridswap/hybridswap.c",
        "static inline meminfo_show(struct hybridswap_stat *stat, char *buf, ssize_t len)",
        "static inline ssize_t meminfo_show(struct hybridswap_stat *stat, char *buf, ssize_t len)",
        "add ssize_t return type to meminfo_show",
        marker="static inline ssize_t meminfo_show(struct hybridswap_stat *stat, char *buf, ssize_t len)",
    )

    # ---------------------------------------------------------------
    # 3. struct mem_cgroup: OPPO-поле android_oem_data1
    # ---------------------------------------------------------------
    print(col(CYN, "[3/7] include/linux/memcontrol.h"))
    p.patch(
        "include/linux/memcontrol.h",
        "\tstruct mem_cgroup_per_node *nodeinfo[0];\n",
        "\t/* OPPO hybridswap per-memcg payload (COS17 port) */\n"
        "\tu64 android_oem_data1;\n"
        "\n"
        "\tstruct mem_cgroup_per_node *nodeinfo[0];\n",
        "android_oem_data1 in struct mem_cgroup",
        marker="u64 android_oem_data1;",
    )

    # ---------------------------------------------------------------
    # 4. mm/memcontrol.c: 5 call sites для hybridswap-хуков
    # ---------------------------------------------------------------
    print(col(CYN, "[4/7] mm/memcontrol.c"))
    p.ensure_include(
        "mm/memcontrol.c",
        "#include <linux/memcontrol.h>",
        "#include <trace/hooks/vh_vmscan.h>",
        "include vh_vmscan.h",
    )

    # 4.1 mem_cgroup_id_remove()
    p.patch(
        "mm/memcontrol.c",
        "\tif (memcg->id.id > 0) {\n"
        "\t\tidr_remove(&mem_cgroup_idr, memcg->id.id);\n",
        "\tif (memcg->id.id > 0) {\n"
        "\t\ttrace_android_vh_mem_cgroup_id_remove(memcg);\n"
        "\t\tidr_remove(&mem_cgroup_idr, memcg->id.id);\n",
        "trace_android_vh_mem_cgroup_id_remove",
        marker="trace_android_vh_mem_cgroup_id_remove(memcg);",
    )

    # 4.2 mem_cgroup_free()
    p.patch(
        "mm/memcontrol.c",
        "static void mem_cgroup_free(struct mem_cgroup *memcg)\n"
        "{\n"
        "\tlru_gen_exit_memcg(memcg);\n",
        "static void mem_cgroup_free(struct mem_cgroup *memcg)\n"
        "{\n"
        "\ttrace_android_vh_mem_cgroup_free(memcg);\n"
        "\tlru_gen_exit_memcg(memcg);\n",
        "trace_android_vh_mem_cgroup_free",
        marker="trace_android_vh_mem_cgroup_free(memcg);",
    )

    # 4.3 mem_cgroup_alloc()
    p.patch(
        "mm/memcontrol.c",
        "\tidr_replace(&mem_cgroup_idr, memcg, memcg->id.id);\n"
        "\tlru_gen_init_memcg(memcg);\n"
        "\treturn memcg;\n",
        "\tidr_replace(&mem_cgroup_idr, memcg, memcg->id.id);\n"
        "\tlru_gen_init_memcg(memcg);\n"
        "\ttrace_android_vh_mem_cgroup_alloc(memcg);\n"
        "\treturn memcg;\n",
        "trace_android_vh_mem_cgroup_alloc",
        marker="trace_android_vh_mem_cgroup_alloc(memcg);",
    )

    # 4.4 mem_cgroup_css_online()
    p.patch(
        "mm/memcontrol.c",
        "\tatomic_set(&memcg->id.ref, 1);\n"
        "\tcss_get(css);\n"
        "\treturn 0;\n",
        "\tatomic_set(&memcg->id.ref, 1);\n"
        "\tcss_get(css);\n"
        "\ttrace_android_vh_mem_cgroup_css_online(css, memcg);\n"
        "\treturn 0;\n",
        "trace_android_vh_mem_cgroup_css_online",
        marker="trace_android_vh_mem_cgroup_css_online(css, memcg);",
    )

    # 4.5 mem_cgroup_css_offline()
    p.patch(
        "mm/memcontrol.c",
        "static void mem_cgroup_css_offline(struct cgroup_subsys_state *css)\n"
        "{\n"
        "\tstruct mem_cgroup *memcg = mem_cgroup_from_css(css);\n"
        "\tstruct mem_cgroup_event *event, *tmp;\n"
        "\n",
        "static void mem_cgroup_css_offline(struct cgroup_subsys_state *css)\n"
        "{\n"
        "\tstruct mem_cgroup *memcg = mem_cgroup_from_css(css);\n"
        "\tstruct mem_cgroup_event *event, *tmp;\n"
        "\n"
        "\ttrace_android_vh_mem_cgroup_css_offline(css, memcg);\n"
        "\n",
        "trace_android_vh_mem_cgroup_css_offline",
        marker="trace_android_vh_mem_cgroup_css_offline(css, memcg);",
    )

    # ---------------------------------------------------------------
    # 5. mm/vmscan.c: swappiness для hybridswapd + tune_scan_type
    # ---------------------------------------------------------------
    print(col(CYN, "[5/7] mm/vmscan.c"))
    # vh_vmscan.h ОБЯЗТЕЛЬНО до `#define CREATE_TRACE_POINTS`:
    # mm/vmscan.c включает trace/events/vmscan.h под CREATE_TRACE_POINTS,
    # и если хуки включены ПОСЛЕ него, __tracepoint_android_vh_* дефайнятся
    # дважды (mm/vmscan.o + drivers/android/vendor_hooks.o) -> duplicate
    # symbol на линке vmlinux.
    p.patch(
        "mm/vmscan.c",
        "#define CREATE_TRACE_POINTS\n#include <trace/events/vmscan.h>\n",
        "#include <trace/hooks/vh_vmscan.h>\n"
        "#define CREATE_TRACE_POINTS\n"
        "#include <trace/events/vmscan.h>\n"
        "#include <linux/string.h>\n"
        "\n"
        "#ifdef CONFIG_HYBRIDSWAP_SWAPD\n"
        "static int hybridswapd_swappiness = 200;\n"
        # free_zram_is_ok: в доноре Realme определён в hybridswapd.c
        # (bool free_zram_is_ok(void)), extern — в hybridswap/internal.h.
        # В оригинале vmscan вызов обёрнут в
        # `#if OPLUS_FEATURE_ZRAM_OPT && CONFIG_OPLUS_ZRAM_OPT` и заменён на
        # free_swap_is_low(); здесь сознательно оставляем упрощённый вариант
        # без зависимости от ZRAM_OPT (он у нас выключен), т.к. free_zram_is_ok
        # существует в обоих донорах.
        "extern bool free_zram_is_ok(void);\n"
        "#endif\n",
        "include vh_vmscan.h before CREATE_TRACE_POINTS + hybridswapd_swappiness",
        marker="static int hybridswapd_swappiness = 200;",
    )
    # Ремонт порядка для деревьев, пропатченных старым вариантом патчера
    # (хуки стояли ПОСЛЕ trace/events/vmscan.h). На чистом дереве ни одного
    # паттерна нет — там правильный порядок ставит include-патч выше.
    _vmscan_rel = "mm/vmscan.c"
    try:
        _v = p._read(_vmscan_rel)
    except FileNotFoundError:
        _v = None
    if _v is not None:
        _bad = ("#define CREATE_TRACE_POINTS\n"
                "#include <trace/events/vmscan.h>\n"
                "#include <linux/string.h>\n"
                "#include <trace/hooks/vh_vmscan.h>\n")
        _good = ("#include <trace/hooks/vh_vmscan.h>\n"
                 "#define CREATE_TRACE_POINTS\n"
                 "#include <trace/events/vmscan.h>\n"
                 "#include <linux/string.h>\n")
        if "\r\n" in _v:
            _bad = _bad.replace("\n", "\r\n")
            _good = _good.replace("\n", "\r\n")
        if _good in _v:
            p.skip("%s: vh_vmscan.h before CREATE_TRACE_POINTS" % _vmscan_rel)
        elif _bad in _v:
            if not p.check:
                p._write(_vmscan_rel, _v.replace(_bad, _good, 1))
            p.ok("%s: vh_vmscan.h before CREATE_TRACE_POINTS" % _vmscan_rel)
    # 5.0 hybridmain.c зовёт move_active_pages_to_lru() напрямую,
    #     а в 4.19.325 она static -> нужен внешний символ.
    p.patch(
        "mm/vmscan.c",
        "static unsigned move_active_pages_to_lru(struct lruvec *lruvec,\n"
        "\t\t\t\t     struct list_head *list,\n"
        "\t\t\t\t     struct list_head *pages_to_free,\n"
        "\t\t\t\t     enum lru_list lru)\n",
        "unsigned move_active_pages_to_lru(struct lruvec *lruvec,\n"
        "\t\t\t\t     struct list_head *list,\n"
        "\t\t\t\t     struct list_head *pages_to_free,\n"
        "\t\t\t\t     enum lru_list lru)\n",
        "de-static move_active_pages_to_lru",
        marker="\nunsigned move_active_pages_to_lru(struct lruvec *lruvec,",
    )
    p.patch(
        "mm/vmscan.c",
        "\tint swappiness = mem_cgroup_swappiness(memcg);\n",
        "\tint swappiness = mem_cgroup_swappiness(memcg);\n"
        "#ifdef CONFIG_HYBRIDSWAP_SWAPD\n"
        "\t/* hybridswapd: zram почти полон -> агрессивно свопаем anon,\n"
        "\t * иначе вообще не трогаем anon (пусть работает zram). */\n"
        "\tif (!current_is_kswapd() &&\n"
        "\t    !strncmp(current->comm, \"hybridswapd:\",\n"
        "\t\t     sizeof(\"hybridswapd:\") - 1)) {\n"
        "\t\tswappiness = hybridswapd_swappiness;\n"
        "\t\tif (!free_zram_is_ok())\n"
        "\t\t\tswappiness = 0;\n"
        "\t}\n"
        "#endif\n",
        "hybridswapd swappiness override",
        marker="swappiness = hybridswapd_swappiness;",
    )
    p.patch(
        "mm/vmscan.c",
        "\t\tif (!scan && !mem_cgroup_online(memcg))\n"
        "\t\t\tscan = min(size, SWAP_CLUSTER_MAX);\n"
        "\n"
        "\t\tswitch (scan_balance) {\n",
        "\t\tif (!scan && !mem_cgroup_online(memcg))\n"
        "\t\t\tscan = min(size, SWAP_CLUSTER_MAX);\n"
        "\n"
        "\t\t/* NOTE: file thp is not supported in kernel-4.19! */\n"
        "\t\ttrace_android_vh_tune_scan_type((char *)(&scan_balance));\n"
        "\t\tswitch (scan_balance) {\n",
        "trace_android_vh_tune_scan_type",
        marker="trace_android_vh_tune_scan_type((char *)(&scan_balance));",
    )

    # ---------------------------------------------------------------
    # 6. mm/page_alloc.c: trace_android_vh_rmqueue (wake_all_swapd)
    # ---------------------------------------------------------------
    print(col(CYN, "[6/7] mm/page_alloc.c"))
    p.ensure_include(
        "mm/page_alloc.c",
        "#include <linux/memcontrol.h>",
        "#include <trace/hooks/vh_vmscan.h>",
        "include vh_vmscan.h",
    )
    p.patch(
        "mm/page_alloc.c",
        "\t__count_zid_vm_events(PGALLOC, page_zonenum(page), 1 << order);\n"
        "\tzone_statistics(preferred_zone, zone);\n"
        "\tlocal_irq_restore(flags);\n",
        "\t__count_zid_vm_events(PGALLOC, page_zonenum(page), 1 << order);\n"
        "\tzone_statistics(preferred_zone, zone);\n"
        "\ttrace_android_vh_rmqueue(preferred_zone, zone, order,\n"
        "\t\t\t\t gfp_flags, alloc_flags, migratetype);\n"
        "\tlocal_irq_restore(flags);\n",
        "trace_android_vh_rmqueue",
        marker="trace_android_vh_rmqueue(preferred_zone, zone, order,",
    )

    # ---------------------------------------------------------------
    # 7. fs/proc/meminfo.c + config fragment
    # ---------------------------------------------------------------
    print(col(CYN, "[7/7] fs/proc/meminfo.c + config"))
    p.ensure_include(
        "fs/proc/meminfo.c",
        "#include <linux/seq_file.h>",
        "#include <trace/hooks/vh_vmscan.h>",
        "include vh_vmscan.h",
    )
    p.patch(
        "fs/proc/meminfo.c",
        "\tarch_report_meminfo(m);\n",
        "\tarch_report_meminfo(m);\n"
        "\ttrace_android_vh_meminfo_proc_show(m);\n",
        "trace_android_vh_meminfo_proc_show",
        marker="trace_android_vh_meminfo_proc_show(m);",
    )
    p.write_if_missing("arch/arm64/configs/vendor/cos17.config", CONFIG_FRAGMENT)

    # ---------------------------------------------------------------
    # 8. include/linux/gfp.h: ___GFP_HIGH_ATOMIC_ZRAM (донорский zram
    #    ждёт его; в 4.19.325 его нет). Realme ставит ту же пару:
    #    define 0x2000000u (бит 25, свободен: CMA — бит 24) + BITS_SHIFT 25->26.
    # ---------------------------------------------------------------
    print(col(CYN, "[8/8] include/linux/gfp.h"))
    p.patch(
        "include/linux/gfp.h",
        "#define ___GFP_CMA\t\t0x1000000u\n",
        "#define ___GFP_CMA\t\t0x1000000u\n"
        "#define ___GFP_HIGH_ATOMIC_ZRAM 0x2000000u\n",
        "___GFP_HIGH_ATOMIC_ZRAM",
        marker="#define ___GFP_HIGH_ATOMIC_ZRAM 0x2000000u",
    )
    p.patch(
        "include/linux/gfp.h",
        "#define __GFP_BITS_SHIFT (25)\n",
        "#define __GFP_BITS_SHIFT (26)\n",
        "__GFP_BITS_SHIFT 25->26",
        marker="#define __GFP_BITS_SHIFT (26)",
    )

    # ---------------------------------------------------------------
    # 9. include/linux/mmzone.h: android_oem_data1 в struct pglist_data.
    #    hybridswapd.c держит per-node hybridswapd_task на pgdat
    #    (PGDAT_ITEM_DATA). В доноре Realme поле стоит после
    #    kswapd_failures, перед #ifdef CONFIG_COMPACTION — ставим так же.
    # ---------------------------------------------------------------
    print(col(CYN, "[9/9] include/linux/mmzone.h"))
    p.patch(
        "include/linux/mmzone.h",
        "\tint kswapd_failures;\t\t/* Number of 'reclaimed == 0' runs */\n"
        "\n"
        "#ifdef CONFIG_COMPACTION\n",
        "\tint kswapd_failures;\t\t/* Number of 'reclaimed == 0' runs */\n"
        "\n"
        "\tu64 android_oem_data1;\n"
        "#ifdef CONFIG_COMPACTION\n",
        "android_oem_data1 in struct pglist_data",
        marker="\tu64 android_oem_data1;\n#ifdef CONFIG_COMPACTION",
    )

    # ---------------------------------------------------------------
    print()
    print(col(CYN, "== summary =="))
    print("   applied : %d" % len(p.applied))
    print("   skipped : %d (already applied)" % len(p.skipped))
    print("   errors  : %d" % len(p.errors))
    if p.errors:
        print()
        for e in p.errors:
            print(col(RED, "  - " + e))
        return 1
    if check:
        print()
        print(col(GRN, "All anchors found. Tree is ready for the patch."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
