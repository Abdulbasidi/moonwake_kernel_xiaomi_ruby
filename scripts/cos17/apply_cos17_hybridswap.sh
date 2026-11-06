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

say "kernel : $ROOT"
say "patcher: $PATCHER"
[ -n "$CONFIG" ] && say "config : $CONFIG"

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
