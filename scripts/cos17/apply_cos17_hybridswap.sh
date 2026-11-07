#!/usr/bin/env bash
# ============================================================================
#  apply_cos17_hybridswap.sh
#
#  Внедряет OPPO hybridswap (integrated zram) в MoonWake 4.19.325 для ruby,
#  чтобы ColorOS 17 мог поднять nandswap (swapfile + loop + hybridswap_core)
#  вместо `persist.sys.oplus.nandswap.err=1004 (ERR_UNSUPPORTED)`.
#
#  Что делает:
#    1) скачивает из донора Realme 9 Pro+ / Narzo 50 Pro 5G (AndroidT,
#       тот же MT6877, Oplus-база) файлы zram/hybridswap
#       и include/trace/hooks/vh_vmscan.h;
#    2) патчит mm/memcontrol.c, mm/vmscan.c, mm/page_alloc.c, fs/proc/meminfo.c,
#       include/linux/memcontrol.h, drivers/android/vendor_hooks.c;
#    3) создаёт arch/arm64/configs/vendor/cos17.config.
#
#  Использование (из корня дерева ядра):
#      bash scripts/cos17/apply_cos17_hybridswap.sh
#      bash scripts/cos17/apply_cos17_hybridswap.sh /path/to/kernel
#      bash scripts/cos17/apply_cos17_hybridswap.sh --check
#
#  Работает и через `curl -Ls .../apply_cos17_hybridswap.sh | bash`
#  (именно так его вызывает KernelAction в шаге "Run Additional Script"):
#  в этом случае корнем считается текущий каталог, а остальные файлы
#  (patch_cos17_kernel.py) ищутся в scripts/cos17/ внутри дерева ядра.
#
#  Зависимости: bash, curl, python3.
# ============================================================================
set -euo pipefail

# --- где лежим -------------------------------------------------------------
# При запуске через `curl | bash` BASH_SOURCE пуст -> SCRIPT_DIR остаётся "".
SELF="${BASH_SOURCE[0]-}"
SCRIPT_DIR=""
if [ -n "$SELF" ] && [ -f "$SELF" ]; then
    SCRIPT_DIR="$(cd "$(dirname "$SELF")" && pwd)"
fi

ROOT=""
CHECK=""
for arg in "$@"; do
    case "$arg" in
        --check)   CHECK="--check" ;;
        -h|--help) sed -n '2,32p' "$SELF" 2>/dev/null || true; exit 0 ;;
        *)         ROOT="$arg" ;;
    esac
done
ROOT="$(cd "${ROOT:-$PWD}" && pwd)"

# --- поиск вспомогательных файлов -----------------------------------------
find_file() {
    local name="$1" c
    if [ -n "$SCRIPT_DIR" ]; then
        c="$SCRIPT_DIR/$name"
        if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
    fi
    c="$ROOT/scripts/cos17/$name"
    if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
    c="$ROOT/$name"
    if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
    return 1
}

# Донор: Realme 9 Pro+ (RMX3392) / Narzo 50 Pro 5G (RMX3395) — AndroidT.
# Тот же SoC MT6877, та же Oplus-база 4.19.191, официальная ветка
# realme-kernel-opensource. Ветка: master (одно коммитное дерево,
# "Upload realme_9pro-plus_narzo50pro 5g AndroidT kernel source").
# SHA зафиксирован, чтобы сборка не зависела от force-push в доноре.
# NOTE: в дереве донора лежит мёртвый hybridswap_stats.c (include
# "hybridswap_internal.h" которого не существует, Makefile его не
# собирает) — его сознательно НЕ качаем.
DONOR_REPO="${DONOR_REPO:-realme-kernel-opensource/realme_9pro-plus_narzo50pro-5g-AndroidT-kernel-source}"
DONOR_REF="${DONOR_REF:-a0105615b00a912a0e77045b6e78574051c97146}"
RAW="https://raw.githubusercontent.com/${DONOR_REPO}/${DONOR_REF}"

say()  { printf '\033[1;36m%s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m%s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- checks ----
[ -f "$ROOT/Makefile" ] || die "not a kernel tree: $ROOT (нет Makefile)"
grep -q '^VERSION = 4' "$ROOT/Makefile" || die "ожидалось ядро 4.x: $ROOT"
grep -q '^PATCHLEVEL = 19' "$ROOT/Makefile" \
    || warn "WARN: это не 4.19.x — донор рассчитан на 4.19.191, возможны расхождения API"
command -v curl    >/dev/null 2>&1 || die "нужен curl"
command -v python3 >/dev/null 2>&1 || die "нужен python3"

PATCHER="$(find_file patch_cos17_kernel.py)" \
    || die "не найден patch_cos17_kernel.py (ожидался рядом со скриптом или в scripts/cos17/)"
CONFIG="$(find_file cos17.config)" || CONFIG=""
CONFIG_SUKISU="$(find_file sukisu.config)" || CONFIG_SUKISU=""

say "kernel : $ROOT"
say "patcher: $PATCHER"
[ -n "$CONFIG" ] && say "config : $CONFIG"
[ -n "$CONFIG_SUKISU" ] && say "sukisu config : $CONFIG_SUKISU"

# ------------------------------------------------------------- download -----
FILES="
drivers/block/zram/Kconfig
drivers/block/zram/Makefile
drivers/block/zram/zcomp.c
drivers/block/zram/zcomp.h
drivers/block/zram/zram_drv.c
drivers/block/zram/zram_drv.h
drivers/block/zram/zram_drv_internal.h
drivers/block/zram/hybridswap/internal.h
drivers/block/zram/hybridswap/hybridmain.c
drivers/block/zram/hybridswap/hybridswap.c
drivers/block/zram/hybridswap/hybridswap.h
drivers/block/zram/hybridswap/hybridswapd.c
include/trace/hooks/vh_vmscan.h
"

fetch() {
    local src="$1" dst="$ROOT/$1"
    mkdir -p "$(dirname "$dst")"
    printf '  fetch  %s\n' "$src"
    curl -fsSL --retry 3 --retry-delay 2 -o "$dst" "${RAW}/${src}" \
        || die "не удалось скачать ${src} (${RAW}/${src})"
}

if [ -z "$CHECK" ]; then
    say "== [1/3] donor sources (${DONOR_REPO}@${DONOR_REF:0:12}) =="
    for f in $FILES; do fetch "$f"; done
    say "== [1/3] done: $(echo "$FILES" | wc -w | tr -d ' ') files =="
else
    say "== [1/3] --check: пропускаю скачивание =="
fi

# --------------------------------------------------------------- patch ------
say "== [2/3] patching kernel tree =="
# shellcheck disable=SC2086
python3 "$PATCHER" "$ROOT" $CHECK

# ------------------------------------------------------- sukisu 4.19 compat --
# SukiSU Ultra местами использует API, появившийся позже 4.19:
#   v4.x (main):  MODULE_IMPORT_NS(), copy_to_kernel_nofault(), fops iopoll/remap
#   v3.2.0:       MODULE_IMPORT_NS(), copy_from_user_nofault(),
#                 policydb_filenametr_search()/filename_trans_key,
#                 path_umount() и GitHub-проба версии в Makefile.
# Без этого clang падает на undeclared identifier / implicit declaration.
# Правим ТОЛЬКО если SukiSU уже развёрнут в дереве (CI делает это до нас).
sukisu_compat() {
    local dir="$ROOT/drivers/kernelsu"
    if [ ! -d "$dir" ]; then
        say "== SukiSU compat: пропускаю (drivers/kernelsu нет) =="
        return 0
    fi
    if [ -f "$dir/infra/file_wrapper.c" ]; then
        say "== SukiSU 4.19 compat (v4.x layout) =="
        python3 - "$dir" <<'PY'
import os
import sys

root = sys.argv[1]

PATCHES = [
    (
        os.path.join("core", "init.c"),
        "#include <linux/moduleparam.h>",
        "#ifndef MODULE_IMPORT_NS",
        "/* COS17: MODULE_IMPORT_NS appeared in Linux 5.4; 4.19 has no such macro. */\n"
        "#ifndef MODULE_IMPORT_NS\n"
        "#define MODULE_IMPORT_NS(ns)\n"
        "#endif",
        "MODULE_IMPORT_NS",
    ),
    (
        os.path.join("hook", "arm64", "patch_memory.c"),
        '#include "asm-generic/fixmap.h"',
        "#define copy_to_kernel_nofault",
        "/* COS17: copy_to_kernel_nofault appeared in Linux 5.8. */\n"
        "#include <linux/version.h>\n"
        "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)\n"
        "#define copy_to_kernel_nofault(dst, src, size) \\\n"
        "\tprobe_kernel_write((dst), (const void *)(src), (size))\n"
        "#endif",
        "copy_to_kernel_nofault",
    ),
]

# (rel, marker, old, new, label) — точечные замены для fops, которых нет в 4.19
EDITS = [
    (
        os.path.join("infra", "file_wrapper.c"),
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)\n#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)",
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)\n"
        "static int ksu_wrapper_iopoll(struct kiocb *kiocb, struct io_comp_batch *icb, unsigned int v)",
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)\n"
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)\n"
        "static int ksu_wrapper_iopoll(struct kiocb *kiocb, struct io_comp_batch *icb, unsigned int v)",
        "iopoll fn guard",
    ),
    (
        os.path.join("infra", "file_wrapper.c"),
        "    return orig->f_op->iopoll(kiocb, spin);\n}\n#endif\n#endif\n",
        "    return orig->f_op->iopoll(kiocb, spin);\n}\n#endif\n",
        "    return orig->f_op->iopoll(kiocb, spin);\n}\n#endif\n#endif\n",
        "iopoll fn end guard",
    ),
    (
        os.path.join("infra", "file_wrapper.c"),
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)\n    p->ops.iopoll",
        "    p->ops.iopoll = fp->f_op->iopoll ? ksu_wrapper_iopoll : NULL;\n",
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)\n"
        "    p->ops.iopoll = fp->f_op->iopoll ? ksu_wrapper_iopoll : NULL;\n"
        "#endif\n",
        "iopoll assignment guard",
    ),
    (
        os.path.join("infra", "file_wrapper.c"),
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)\nstatic loff_t ksu_wrapper_remap_file_range",
        "static loff_t ksu_wrapper_remap_file_range(struct file *file_in, loff_t pos_in, struct file *file_out, loff_t pos_out,\n",
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)\n"
        "static loff_t ksu_wrapper_remap_file_range(struct file *file_in, loff_t pos_in, struct file *file_out, loff_t pos_out,\n",
        "remap fn guard",
    ),
    (
        os.path.join("infra", "file_wrapper.c"),
        "}\n#endif\n\nstatic int ksu_wrapper_fadvise",
        "        return orig->f_op->remap_file_range(orig, pos_in, file_out, pos_out, len, remap_flags);\n"
        "    }\n"
        "}\n",
        "        return orig->f_op->remap_file_range(orig, pos_in, file_out, pos_out, len, remap_flags);\n"
        "    }\n"
        "}\n"
        "#endif\n",
        "remap fn end guard",
    ),
    (
        os.path.join("infra", "file_wrapper.c"),
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)\n    p->ops.remap_file_range",
        "    p->ops.remap_file_range = fp->f_op->remap_file_range ? ksu_wrapper_remap_file_range : NULL;\n",
        "#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)\n"
        "    p->ops.remap_file_range = fp->f_op->remap_file_range ? ksu_wrapper_remap_file_range : NULL;\n"
        "#endif\n",
        "remap assignment guard",
    ),
]

fail = 0
for rel, anchor, marker, block, label in PATCHES:
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        print("  skip: %s not found" % rel)
        continue
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read()
    if marker in src:
        print("  skip: %s (%s compat already present)" % (rel, label))
        continue
    if anchor not in src:
        print("  FAIL: anchor '%s' not found in %s" % (anchor, rel))
        fail = 1
        continue
    src = src.replace(anchor, anchor + "\n\n" + block, 1)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    print("  ok: %s compat added to %s" % (label, rel))

for rel, marker, old, new, label in EDITS:
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        print("  skip: %s not found" % rel)
        continue
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read()
    if marker in src:
        print("  skip: %s (%s already present)" % (rel, label))
        continue
    n = src.count(old)
    if n != 1:
        print("  FAIL: %s: anchor matched %d time(s), expected 1" % (rel, n))
        fail = 1
        continue
    src = src.replace(old, new, 1)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    print("  ok: %s added to %s" % (label, rel))

sys.exit(fail)
PY
    elif [ -f "$dir/core_hook.c" ]; then
        say "== SukiSU 4.19 compat (v3.2.0 layout) =="
        python3 - "$dir" <<'PY'
import os
import sys

root = sys.argv[1]

PATCHES = [
    (
        "ksu.c",
        "#include <linux/workqueue.h>",
        "#define MODULE_IMPORT_NS(ns)",
        "/* COS17: MODULE_IMPORT_NS appeared in Linux 5.4; 4.19 has no such macro. */\n"
        "#ifndef MODULE_IMPORT_NS\n"
        "#define MODULE_IMPORT_NS(ns)\n"
        "#endif",
        "MODULE_IMPORT_NS",
    ),
    (
        "kernel_compat.h",
        "#include <linux/version.h>",
        "#define copy_from_user_nofault",
        "/* COS17: copy_from_user_nofault appeared in Linux 5.8; 4.19 has probe_user_read(). */\n"
        "#include <linux/uaccess.h>\n"
        "#ifndef copy_from_user_nofault\n"
        "#define copy_from_user_nofault(dst, src, size) \\\n"
        "\tprobe_user_read((dst), (const void __user *)(src), (size))\n"
        "#endif",
        "copy_from_user_nofault",
    ),
    (
        "Makefile",
        "$(info -- $(REPO_NAME) version (Github): $(KSU_VERSION_FULL))\nendif",
        "KSU_VERSION := 13375",
        "# COS17: the v3.2.0 Makefile probes GitHub and would report the latest\n"
        "# v4.x release plus a main-branch commit count, making the manager\n"
        "# enable post-v3.2.0 features. Pin the v3.2.0 release version instead.\n"
        "KSU_VERSION := 13375\n"
        "KSU_VERSION_FULL := v3.2.0\n"
        "# COS17: 4.19's security/selinux/include/objsec.h already provides\n"
        "# current_sid(), so tell selinux.c not to define it again.\n"
        "ccflags-y += -DKSU_COMPAT_HAS_CURRENT_SID",
        "KSU_VERSION pin",
    ),
    (
        "selinux/sepolicy.c",
        "\t// kfree(old);\n\treturn new;\n}",
        "COS17: 4.19 stores the type/sym maps",
        "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)\n"
        "/* COS17: 4.19 stores the type/sym maps in struct flex_array; grow them\n"
        " * by allocating a new array and copying the elements. The old array is\n"
        " * intentionally leaked, same policy as ksu_realloc() above.\n"
        " */\n"
        "static struct flex_array *ksu_flex_array_grow(struct flex_array *old,\n"
        "\t\t\t\t\t      int element_size,\n"
        "\t\t\t\t\t      unsigned int old_nr,\n"
        "\t\t\t\t\t      unsigned int new_nr)\n"
        "{\n"
        "\tstruct flex_array *new;\n"
        "\tunsigned int i;\n"
        "\n"
        "\tnew = flex_array_alloc(element_size, new_nr, GFP_ATOMIC);\n"
        "\tif (!new)\n"
        "\t\treturn NULL;\n"
        "\tif (flex_array_prealloc(new, 0, new_nr, GFP_ATOMIC)) {\n"
        "\t\tflex_array_free(new);\n"
        "\t\treturn NULL;\n"
        "\t}\n"
        "\tfor (i = 0; i < old_nr; i++) {\n"
        "\t\tvoid *src = flex_array_get(old, i);\n"
        "\t\tvoid *dst = flex_array_get(new, i);\n"
        "\n"
        "\t\tif (!src || !dst) {\n"
        "\t\t\tflex_array_free(new);\n"
        "\t\t\treturn NULL;\n"
        "\t\t}\n"
        "\t\tmemcpy(dst, src, element_size);\n"
        "\t}\n"
        "\treturn new;\n"
        "}\n"
        "#endif\n"
        "\n"
        "static struct ebitmap *ksu_type_attr_map(struct policydb *db, u32 index)\n"
        "{\n"
        "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)\n"
        "\treturn flex_array_get(db->type_attr_map_array, index);\n"
        "#else\n"
        "\treturn &db->type_attr_map_array[index];\n"
        "#endif\n"
        "}",
        "flex_array type/sym maps",
    ),
]

fail = 0
for rel, anchor, marker, block, label in PATCHES:
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        print("  skip: %s not found" % rel)
        continue
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read().replace("\r\n", "\n")
    if marker in src:
        print("  skip: %s (%s already present)" % (rel, label))
        continue
    if anchor not in src:
        print("  FAIL: anchor not found in %s (%s)" % (rel, label))
        fail = 1
        continue
    src = src.replace(anchor, anchor + "\n\n" + block, 1)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    print("  ok: %s added to %s" % (label, rel))

REPLACES = [
    (
        os.path.join("core_hook.c"),
        "COS17: 4.19 declares path_umount",
        "static void ksu_umount_mnt(struct path *path, int flags)\n{",
        "/* COS17: 4.19 declares path_umount() only in fs/internal.h. */\n"
        "extern int path_umount(struct path *path, int flags);\n"
        "\n"
        "static void ksu_umount_mnt(struct path *path, int flags)\n{",
        "path_umount declaration",
    ),
    (
        os.path.join("throne_comm.c"),
        "COS17: struct proc_ops appeared",
        "static const struct proc_ops uid_scanner_proc_ops = {\n"
        "    .proc_open = uid_scanner_open,\n"
        "    .proc_read = seq_read,\n"
        "\t.proc_write = uid_scanner_write,\n"
        "    .proc_lseek = seq_lseek,\n"
        "    .proc_release = single_release,\n"
        "};",
        "/* COS17: struct proc_ops appeared in Linux 5.6; 4.19 uses file_operations. */\n"
        "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0)\n"
        "static const struct file_operations uid_scanner_proc_ops = {\n"
        "\t.open = uid_scanner_open,\n"
        "\t.read = seq_read,\n"
        "\t.write = uid_scanner_write,\n"
        "\t.llseek = seq_lseek,\n"
        "\t.release = single_release,\n"
        "};\n"
        "#else\n"
        "static const struct proc_ops uid_scanner_proc_ops = {\n"
        "\t.proc_open = uid_scanner_open,\n"
        "\t.proc_read = seq_read,\n"
        "\t.proc_write = uid_scanner_write,\n"
        "\t.proc_lseek = seq_lseek,\n"
        "\t.proc_release = single_release,\n"
        "};\n"
        "#endif",
        "proc_ops from file_operations",
    ),
    (
        os.path.join("selinux", "sepolicy.c"),
        "COS17: 4.19 keeps the type/sym maps",
        "\tstruct ebitmap *new_type_attr_map_array =\n"
        "\t\tksu_realloc(db->type_attr_map_array,\n"
        "\t\t\t    value * sizeof(struct ebitmap),\n"
        "\t\t\t    (value - 1) * sizeof(struct ebitmap));\n"
        "\n"
        "\tif (!new_type_attr_map_array) {\n"
        "\t\tpr_err(\"add_type: alloc type_attr_map_array failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tstruct type_datum **new_type_val_to_struct =\n"
        "\t\tksu_realloc(db->type_val_to_struct,\n"
        "\t\t\t    sizeof(*db->type_val_to_struct) * value,\n"
        "\t\t\t    sizeof(*db->type_val_to_struct) * (value - 1));\n"
        "\n"
        "\tif (!new_type_val_to_struct) {\n"
        "\t\tpr_err(\"add_type: alloc type_val_to_struct failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tchar **new_val_to_name_types =\n"
        "\t\tksu_realloc(db->sym_val_to_name[SYM_TYPES],\n"
        "\t\t\t    sizeof(char *) * value,\n"
        "\t\t\t    sizeof(char *) * (value - 1));\n"
        "\tif (!new_val_to_name_types) {\n"
        "\t\tpr_err(\"add_type: alloc val_to_name failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tdb->type_attr_map_array = new_type_attr_map_array;\n"
        "\tebitmap_init(&db->type_attr_map_array[value - 1]);\n"
        "\tebitmap_set_bit(&db->type_attr_map_array[value - 1], value - 1, 1);\n"
        "\n"
        "\tdb->type_val_to_struct = new_type_val_to_struct;\n"
        "\tdb->type_val_to_struct[value - 1] = type;\n"
        "\n"
        "\tdb->sym_val_to_name[SYM_TYPES] = new_val_to_name_types;\n"
        "\tdb->sym_val_to_name[SYM_TYPES][value - 1] = key;",
        "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)\n"
        "\t/* COS17: 4.19 keeps the type/sym maps in flex arrays. */\n"
        "\tstruct flex_array *new_type_attr_map_array =\n"
        "\t\tksu_flex_array_grow(db->type_attr_map_array,\n"
        "\t\t\t\t    sizeof(struct ebitmap), value - 1, value);\n"
        "\n"
        "\tif (!new_type_attr_map_array) {\n"
        "\t\tpr_err(\"add_type: alloc type_attr_map_array failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tstruct flex_array *new_type_val_to_struct_array =\n"
        "\t\tksu_flex_array_grow(db->type_val_to_struct_array,\n"
        "\t\t\t\t    sizeof(struct type_datum *), value - 1,\n"
        "\t\t\t\t    value);\n"
        "\n"
        "\tif (!new_type_val_to_struct_array) {\n"
        "\t\tpr_err(\"add_type: alloc type_val_to_struct failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tstruct flex_array *new_val_to_name_types =\n"
        "\t\tksu_flex_array_grow(db->sym_val_to_name[SYM_TYPES],\n"
        "\t\t\t\t    sizeof(char *), value - 1, value);\n"
        "\tif (!new_val_to_name_types) {\n"
        "\t\tpr_err(\"add_type: alloc val_to_name failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tdb->type_attr_map_array = new_type_attr_map_array;\n"
        "\t{\n"
        "\t\tstruct ebitmap *map =\n"
        "\t\t\tksu_type_attr_map(db, value - 1);\n"
        "\t\tif (!map) {\n"
        "\t\t\tpr_err(\"add_type: get type_attr_map failed\\n\");\n"
        "\t\t\treturn false;\n"
        "\t\t}\n"
        "\t\tebitmap_init(map);\n"
        "\t\tebitmap_set_bit(map, value - 1, 1);\n"
        "\t}\n"
        "\n"
        "\tdb->type_val_to_struct_array = new_type_val_to_struct_array;\n"
        "\tflex_array_put_ptr(db->type_val_to_struct_array, value - 1, type,\n"
        "\t\t\t   GFP_ATOMIC);\n"
        "\n"
        "\tdb->sym_val_to_name[SYM_TYPES] = new_val_to_name_types;\n"
        "\tflex_array_put_ptr(db->sym_val_to_name[SYM_TYPES], value - 1, key,\n"
        "\t\t\t   GFP_ATOMIC);\n"
        "#else\n"
        "\tstruct ebitmap *new_type_attr_map_array =\n"
        "\t\tksu_realloc(db->type_attr_map_array,\n"
        "\t\t\t    value * sizeof(struct ebitmap),\n"
        "\t\t\t    (value - 1) * sizeof(struct ebitmap));\n"
        "\n"
        "\tif (!new_type_attr_map_array) {\n"
        "\t\tpr_err(\"add_type: alloc type_attr_map_array failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tstruct type_datum **new_type_val_to_struct =\n"
        "\t\tksu_realloc(db->type_val_to_struct,\n"
        "\t\t\t    sizeof(*db->type_val_to_struct) * value,\n"
        "\t\t\t    sizeof(*db->type_val_to_struct) * (value - 1));\n"
        "\n"
        "\tif (!new_type_val_to_struct) {\n"
        "\t\tpr_err(\"add_type: alloc type_val_to_struct failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tchar **new_val_to_name_types =\n"
        "\t\tksu_realloc(db->sym_val_to_name[SYM_TYPES],\n"
        "\t\t\t    sizeof(char *) * value,\n"
        "\t\t\t    sizeof(char *) * (value - 1));\n"
        "\tif (!new_val_to_name_types) {\n"
        "\t\tpr_err(\"add_type: alloc val_to_name failed\\n\");\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\n"
        "\tdb->type_attr_map_array = new_type_attr_map_array;\n"
        "\tebitmap_init(&db->type_attr_map_array[value - 1]);\n"
        "\tebitmap_set_bit(&db->type_attr_map_array[value - 1], value - 1, 1);\n"
        "\n"
        "\tdb->type_val_to_struct = new_type_val_to_struct;\n"
        "\tdb->type_val_to_struct[value - 1] = type;\n"
        "\n"
        "\tdb->sym_val_to_name[SYM_TYPES] = new_val_to_name_types;\n"
        "\tdb->sym_val_to_name[SYM_TYPES][value - 1] = key;\n"
        "#endif",
        "type/sym maps for 4.19 flex_array",
    ),
    (
        os.path.join("selinux", "sepolicy.c"),
        "COS17: ksu_type_attr_map",
        "\tstruct ebitmap *sattr = &db->type_attr_map_array[type->value - 1];",
        "\t/* COS17: ksu_type_attr_map: 4.19 keeps type_attr_map_array in a flex_array. */\n"
        "\tstruct ebitmap *sattr = ksu_type_attr_map(db, type->value - 1);",
        "type_attr_map lookup",
    ),
    (
        os.path.join("selinux", "rules.c"),
        "COS17: 4.19 keeps the policydb",
        "static struct policydb *get_policydb(void)\n"
        "{\n"
        "\tstruct policydb *db;\n"
        "\tstruct selinux_policy *policy = selinux_state.policy;\n"
        "\tdb = &policy->policydb;\n"
        "\treturn db;\n"
        "}",
        "static struct policydb *get_policydb(void)\n"
        "{\n"
        "\tstruct policydb *db;\n"
        "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)\n"
        "\t/* COS17: 4.19 keeps the policydb in selinux_state.ss. */\n"
        "\tstruct selinux_ss *ss = selinux_state.ss;\n"
        "\tdb = &ss->policydb;\n"
        "#else\n"
        "\tstruct selinux_policy *policy = selinux_state.policy;\n"
        "\tdb = &policy->policydb;\n"
        "#endif\n"
        "\treturn db;\n"
        "}",
        "policydb from selinux_state.ss",
    ),
]

for rel, marker, old, new, label in REPLACES:
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        print("  skip: %s not found" % rel)
        continue
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read().replace("\r\n", "\n")
    if marker in src:
        print("  skip: %s (%s already present)" % (rel, label))
        continue
    if src.count(old) != 1:
        print("  FAIL: %s: anchor matched %d time(s), expected 1 (%s)"
              % (rel, src.count(old), label))
        fail = 1
        continue
    src = src.replace(old, new, 1)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    print("  ok: %s replaced in %s" % (label, rel))

# --- filename_trans: 4.19 uses the pre-5.7 policydb layout ------------------
rel = os.path.join("selinux", "sepolicy.c")
path = os.path.join(root, rel)
if not os.path.isfile(path):
    print("  skip: %s not found" % rel)
else:
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read().replace("\r\n", "\n")
    if "COS17: 4.19 filename_trans" in src:
        print("  skip: %s (filename_trans compat already present)" % rel)
    else:
        start_marker = "\tstruct filename_trans_key key;\n"
        end_marker = "\treturn ebitmap_set_bit(&trans->stypes, src->value - 1, 1) == 0;\n"
        i = src.find(start_marker)
        j = src.find(end_marker, i + 1) if i >= 0 else -1
        if i < 0 or j < 0:
            print("  FAIL: filename_trans anchors not found in %s" % rel)
            fail = 1
        else:
            body = src[i:j + len(end_marker)]
            compat = (
                "#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)\n"
                "\t/* COS17: 4.19 filename_trans is keyed by stype/ttype/tclass/name\n"
                "\t * and the datum only stores otype; there is no stypes bitmap\n"
                "\t * and no compat_filename_trans_count in struct policydb.\n"
                "\t */\n"
                "\tstruct filename_trans key;\n"
                "\tstruct filename_trans_datum *trans;\n"
                "\tstruct filename_trans *new_key;\n"
                "\n"
                "\tkey.stype = src->value;\n"
                "\tkey.ttype = tgt->value;\n"
                "\tkey.tclass = cls->value;\n"
                "\tkey.name = (char *)o;\n"
                "\n"
                "\ttrans = hashtab_search(db->filename_trans, &key);\n"
                "\tif (trans == NULL) {\n"
                "\t\ttrans = (struct filename_trans_datum *)kzalloc(\n"
                "\t\t\tsizeof(*trans), GFP_ATOMIC);\n"
                "\t\tnew_key = (struct filename_trans *)kmalloc(\n"
                "\t\t\tsizeof(*new_key), GFP_ATOMIC);\n"
                "\t\tif (!trans || !new_key) {\n"
                "\t\t\tkfree(trans);\n"
                "\t\t\tkfree(new_key);\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t\t*new_key = key;\n"
                "\t\tnew_key->name = kstrdup(key.name, GFP_ATOMIC);\n"
                "\t\tif (!new_key->name) {\n"
                "\t\t\tkfree(new_key);\n"
                "\t\t\tkfree(trans);\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t\tif (hashtab_insert(db->filename_trans, new_key, trans)) {\n"
                "\t\t\tkfree(new_key->name);\n"
                "\t\t\tkfree(new_key);\n"
                "\t\t\tkfree(trans);\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t} else {\n"
                "\t\ttrans->otype = def->value;\n"
                "\t}\n"
                "\n"
                "\tebitmap_set_bit(&db->filename_trans_ttypes, key.ttype, 1);\n"
                "\treturn true;\n"
                "#else\n"
            )
            src = src[:i] + compat + body + "#endif\n" + src[j + len(end_marker):]
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(src)
            print("  ok: filename_trans pre-5.7 branch added to %s" % rel)

sys.exit(fail)
PY
    else
        warn "== SukiSU compat: неизвестная раскладка, пропускаю =="
    fi
}

if [ -z "$CHECK" ]; then
    sukisu_compat
else
    say "== --check: SukiSU compat не применяю =="
fi

# ---------------------------------------------------------------- config ----
if [ -z "$CHECK" ]; then
    say "== [3/3] config fragment =="
    DEST="$ROOT/arch/arm64/configs/vendor/cos17.config"
    if [ -n "$CONFIG" ] && [ -f "$CONFIG" ] && [ ! -f "$DEST" ]; then
        mkdir -p "$(dirname "$DEST")"
        cp "$CONFIG" "$DEST"
        printf '  write  %s\n' "${DEST#$ROOT/}"
    elif [ -f "$DEST" ]; then
        printf '  keep   %s (уже есть)\n' "${DEST#$ROOT/}"
    else
        warn "  WARN: cos17.config рядом не найден, но патчер должен был его создать"
    fi
    DEST_SUKISU="$ROOT/arch/arm64/configs/vendor/sukisu.config"
    if [ -n "$CONFIG_SUKISU" ] && [ -f "$CONFIG_SUKISU" ] && [ ! -f "$DEST_SUKISU" ]; then
        mkdir -p "$(dirname "$DEST_SUKISU")"
        cp "$CONFIG_SUKISU" "$DEST_SUKISU"
        printf '  write  %s\n' "${DEST_SUKISU#$ROOT/}"
    elif [ -f "$DEST_SUKISU" ]; then
        printf '  keep   %s (уже есть)\n' "${DEST_SUKISU#$ROOT/}"
    fi
else
    say "== [3/3] --check: конфиг не пишу =="
fi

# ---------------------------------------------------------------- verify ----
if [ -z "$CHECK" ]; then
    say "== verify =="
    miss=0
    chk() {
        if { [ -n "$1" ] && [ -e "$ROOT/$1" ]; } \
           || { [ -n "$2" ] && grep -qF -- "$2" "$ROOT/$3" 2>/dev/null; }; then
            printf '  ok     %s\n' "$4"
        else
            printf '\033[1;31m  MISS   %s\033[0m\n' "$4"; miss=$((miss+1))
        fi
    }
    chk drivers/block/zram/hybridswap/hybridmain.c "" "hybridmain.c" "zram hybridswap sources"
    chk include/trace/hooks/vh_vmscan.h "" "vh_vmscan.h" "vendor hooks header"
    chk "" "u64 android_oem_data1;" include/linux/memcontrol.h "memcg android_oem_data1"
    chk "" "trace_android_vh_mem_cgroup_alloc(memcg);" mm/memcontrol.c "memcg hook: alloc"
    chk "" "trace_android_vh_mem_cgroup_css_offline(css, memcg);" mm/memcontrol.c "memcg hook: css_offline"
    chk "" "hybridswapd_swappiness" mm/vmscan.c "vmscan hybridswapd swappiness"
    chk "" "trace_android_vh_tune_scan_type" mm/vmscan.c "vmscan tune_scan_type"
    chk "" "trace_android_vh_rmqueue" mm/page_alloc.c "page_alloc rmqueue hook"
    chk "" "trace_android_vh_meminfo_proc_show(m);" fs/proc/meminfo.c "meminfo hook"
    chk "" "#define ___GFP_HIGH_ATOMIC_ZRAM 0x2000000u" include/linux/gfp.h "gfp HIGH_ATOMIC_ZRAM"
    chk "" "u64 android_oem_data1;" include/linux/mmzone.h "pgdat android_oem_data1"
    chk drivers/misc/oplus_nodes.c "" "" "oplus_nodes driver"
    chk "" "CONFIG_OPLUS_NODES=y" arch/arm64/configs/vendor/cos17.config "OPLUS_NODES config"
    chk "" "#include <trace/hooks/vh_vmscan.h>" drivers/android/vendor_hooks.c "CREATE_TRACE_POINTS"
    chk arch/arm64/configs/vendor/cos17.config "" "" "config fragment"

    if grep -qE '^unsigned move_active_pages_to_lru' "$ROOT/mm/vmscan.c" 2>/dev/null; then
        printf '  ok     %s\n' "de-static move_active_pages_to_lru"
    else
        printf '\033[1;31m  MISS   %s\033[0m\n' "de-static move_active_pages_to_lru"
        miss=$((miss+1))
    fi
    if grep -qF 'COS17: hybridswap needs' "$ROOT/drivers/block/zram/hybridswap/hybridmain.c" 2>/dev/null; then
        printf '  ok     %s\n' "hybridmain vendor hooks guard"
    else
        printf '\033[1;31m  MISS   %s\033[0m\n' "hybridmain vendor hooks guard"
        miss=$((miss+1))
    fi
    if [ "$miss" -ne 0 ]; then
        die "verify failed: $miss check(s) missing"
    fi
    say "== done: дерево готово к сборке =="
    cat <<'EOF'

Дальше:
  1) git add -A && git commit -m "cos17: integrate OPPO hybridswap (zram + nandswap)"
  2) push в ветку cos17
  3) в CI (KernelAction) собрать конфиг configs/release/cos17.json
     ВАЖНО: vendor/cos17.config должен идти ПОСЛЕДНИМ в extraConfig,
     иначе slmk.config (MEMCG=n) всё отменит.
EOF
else
    say "== --check ok: анкоры на месте =="
fi
