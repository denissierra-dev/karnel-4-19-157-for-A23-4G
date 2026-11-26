#!/sbin/sh
# Idempotent TWRP/recovery patcher for PHH/LineageOS GSI runtime scripts
# Target device: Samsung Galaxy A23 4G (SM-A235M / a23ub) on stock Android 11 vendor
#
# Usage in TWRP (Mount -> System checked, "Mount system partition read-only" unchecked):
#   sh /sdcard/patch_rw_system_gsi.sh [--all | --c2-only | --bright-only | --sim-only | --hotspot-only] [--target PATH] [--sim-target PATH] [--boot-target PATH]

set -eu

DO_C2=1
DO_BRIGHT=1
DO_SIM=1
DO_HOTSPOT=1
CUSTOM_TARGET=""
CUSTOM_SIM_TARGET=""
CUSTOM_BOOT_TARGET=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --all)
            DO_C2=1
            DO_BRIGHT=1
            DO_SIM=1
            DO_HOTSPOT=1
            ;;
        --c2-only)
            DO_C2=1
            DO_BRIGHT=0
            DO_SIM=0
            DO_HOTSPOT=0
            ;;
        --bright-only)
            DO_C2=0
            DO_BRIGHT=1
            DO_SIM=0
            DO_HOTSPOT=0
            ;;
        --sim-only)
            DO_C2=0
            DO_BRIGHT=0
            DO_SIM=1
            DO_HOTSPOT=0
            ;;
        --hotspot-only)
            DO_C2=0
            DO_BRIGHT=0
            DO_SIM=0
            DO_HOTSPOT=1
            ;;
        --target)
            shift
            [ "$#" -gt 0 ] || { echo "ERROR: --target requires a path"; exit 1; }
            CUSTOM_TARGET="$1"
            ;;
        --sim-target)
            shift
            [ "$#" -gt 0 ] || { echo "ERROR: --sim-target requires a path"; exit 1; }
            CUSTOM_SIM_TARGET="$1"
            ;;
        --boot-target)
            shift
            [ "$#" -gt 0 ] || { echo "ERROR: --boot-target requires a path"; exit 1; }
            CUSTOM_BOOT_TARGET="$1"
            ;;
        -h|--help)
            sed -n '2,7p' "$0"
            exit 0
            ;;
        *)
            echo "ERROR: Unknown argument: $1"
            exit 1
            ;;
    esac
    shift
done

find_file() {
    override="$1"
    name="$2"
    if [ -n "$override" ]; then
        echo "$override"
        return 0
    fi
    for p in "/system_root/system/bin/$name" "/system/system/bin/$name" "/system/bin/$name"; do
        if [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

write_inplace() {
    src="$1"
    dst="$2"
    sh -n "$src"
    # Truncate/write in-place to preserve existing inode, mode, and SELinux context.
    cat "$src" > "$dst"
    chmod 0755 "$dst"
}

TARGET=""
SIM_TARGET=""
BOOT_TARGET=""
SYS_MOUNT=""

if [ "$DO_C2" -eq 1 ] || [ "$DO_BRIGHT" -eq 1 ]; then
    TARGET="$(find_file "$CUSTOM_TARGET" "rw-system.sh" || true)"
    [ -n "$TARGET" ] && [ -f "$TARGET" ] || { echo "ERROR: rw-system.sh not found (mount System in TWRP first)"; exit 1; }
fi

if [ "$DO_SIM" -eq 1 ]; then
    SIM_TARGET="$(find_file "$CUSTOM_SIM_TARGET" "phh-on-data.sh" || true)"
    [ -n "$SIM_TARGET" ] && [ -f "$SIM_TARGET" ] || { echo "ERROR: phh-on-data.sh not found (mount System in TWRP first)"; exit 1; }
fi

if [ "$DO_HOTSPOT" -eq 1 ]; then
    BOOT_TARGET="$(find_file "$CUSTOM_BOOT_TARGET" "phh-on-boot.sh" || true)"
    [ -n "$BOOT_TARGET" ] && [ -f "$BOOT_TARGET" ] || { echo "ERROR: phh-on-boot.sh not found (mount System in TWRP first)"; exit 1; }
fi

if [ -z "$CUSTOM_TARGET" ] && [ -z "$CUSTOM_SIM_TARGET" ] && [ -z "$CUSTOM_BOOT_TARGET" ]; then
    probe="${TARGET:-${SIM_TARGET:-$BOOT_TARGET}}"
    case "$probe" in
        /system_root/*) SYS_MOUNT="/system_root" ;;
        /system/system/*) SYS_MOUNT="/system" ;;
        *) SYS_MOUNT="/system" ;;
    esac
    mount -o remount,rw "$SYS_MOUNT" 2>/dev/null || true
    for f in "$TARGET" "$SIM_TARGET" "$BOOT_TARGET"; do
        [ -z "$f" ] && continue
        [ -w "$f" ] || { echo "ERROR: $f is read-only; uncheck read-only in TWRP Mount"; exit 1; }
    done
    [ -n "$TARGET" ] && [ ! -f /sdcard/rw-system.sh.bak ] && cp -a "$TARGET" /sdcard/rw-system.sh.bak
    [ -n "$SIM_TARGET" ] && [ ! -f /sdcard/phh-on-data.sh.bak ] && cp -a "$SIM_TARGET" /sdcard/phh-on-data.sh.bak
    [ -n "$BOOT_TARGET" ] && [ ! -f /sdcard/phh-on-boot.sh.bak ] && cp -a "$BOOT_TARGET" /sdcard/phh-on-boot.sh.bak
fi

WORK="/tmp/rw-system.gsi.$$.tmp"
NEXT="/tmp/rw-system.gsi.$$.next"
SIM_WORK="/tmp/phh-on-data.gsi.$$.tmp"
BOOT_WORK="/tmp/phh-on-boot.gsi.$$.tmp"
trap 'rm -f "$WORK" "$NEXT" "$SIM_WORK" "$BOOT_WORK"' EXIT

ANY_CHANGED=0

if [ -n "$TARGET" ]; then
    cp -a "$TARGET" "$WORK"
    RW_CHANGED=0

    if [ "$DO_C2" -eq 1 ]; then
        if grep -q 'samsung\.software\.media\.c2-base-policy' "$WORK"; then
            echo "C2_SECCOMP: already patched"
        else
            awk '
            {
                print $0
                if (!done && index($0, "codec2.vendor.base.policy") > 0) {
                    in_block = 1
                } else if (in_block && $0 == "fi") {
                    print ""
                    print "if [ -f /vendor/etc/seccomp_policy/samsung.software.media.c2-base-policy ];then"
                    print "    ctxt=\"$(ls -lZ /vendor/etc/seccomp_policy/samsung.software.media.c2-base-policy | grep -oE '\''u:object_r:[^:]*:s0'\'')\""
                    print "    cp -a /vendor/etc/seccomp_policy/samsung.software.media.c2-base-policy /mnt/phh/sec_c2_base.policy"
                    print "    sed -i '\''s/^mremap: arg3 == 3$/mremap: arg3 == 3 || arg3 == MREMAP_MAYMOVE/'\'' /mnt/phh/sec_c2_base.policy"
                    print "    printf '\''\\nfsetxattr: 1\\nsetsockopt: 1\\nsendmsg: 1\\nrecvfrom: 1\\nuname: 1\\nprlimit64: 1\\nstatx: 1\\nclone3: 1\\nrseq: 1\\nmembarrier: 1\\nsched_getaffinity: 1\\nsched_setaffinity: 1\\nsched_setscheduler: 1\\nsched_get_priority_max: 1\\nsched_get_priority_min: 1\\ngetpriority: 1\\ngetsockopt: 1\\nsocket: 1\\nconnect: 1\\nfcntl: 1\\nclock_nanosleep: 1\\neventfd2: 1\\nepoll_create1: 1\\nepoll_ctl: 1\\nepoll_pwait: 1\\n'\'' >> /mnt/phh/sec_c2_base.policy"
                    print "    chmod 0644 /mnt/phh/sec_c2_base.policy"
                    print "    [ -n \"$ctxt\" ] && chcon \"$ctxt\" /mnt/phh/sec_c2_base.policy"
                    print "    mount -o bind /mnt/phh/sec_c2_base.policy /vendor/etc/seccomp_policy/samsung.software.media.c2-base-policy"
                    print "fi"
                    in_block = 0
                    done = 1
                }
            }
            END {
                if (!done) exit 2
            }
            ' "$WORK" > "$NEXT"
            mv "$NEXT" "$WORK"
            RW_CHANGED=1
            echo "C2_SECCOMP: staged"
        fi
    fi

    if [ "$DO_BRIGHT" -eq 1 ]; then
        if grep -q 'samsung/a23' "$WORK"; then
            echo "BRIGHTNESS: already patched"
        else
            awk '
            {
                print $0
                if (!done && index($0, "setprop persist.sys.qcom-brightness \"$(cat /sys/class/backlight/panel0-backlight/max_brightness)\"") > 0) {
                    in_block = 1
                } else if (in_block && $0 == "fi") {
                    print ""
                    print "# Samsung A23 uses SehLight AIDL (0..255 grayscale RGB when full_brightness=false)"
                    print "if getprop ro.vendor.build.fingerprint | grep -qiE '\''^samsung/a23'\''; then"
                    print "    setprop persist.sys.qcom-brightness -1"
                    print "fi"
                    in_block = 0
                    done = 1
                }
            }
            END {
                if (!done) exit 2
            }
            ' "$WORK" > "$NEXT"
            mv "$NEXT" "$WORK"
            RW_CHANGED=1
            echo "BRIGHTNESS: staged"
        fi
    fi

    if [ "$RW_CHANGED" -eq 1 ]; then
        write_inplace "$WORK" "$TARGET"
        ANY_CHANGED=1
    fi
fi

if [ -n "$SIM_TARGET" ] && [ "$DO_SIM" -eq 1 ]; then
    if grep -q 'persist\.radio\.uicc\.enablement0' "$SIM_TARGET"; then
        echo "SIM_UICC: already patched"
    else
        cp -a "$SIM_TARGET" "$SIM_WORK"
        cat << 'PATCH' >> "$SIM_WORK"

# Recover Samsung A23 RIL UICC slot 0 if toggled off from AOSP UI (prevents permanent NOT_READY)
if getprop ro.vendor.build.fingerprint | grep -qiE '^samsung/a23'; then
    if [ "$(getprop persist.radio.uicc.enablement0)" = "0" ]; then
        setprop persist.radio.uicc.enablement0 1
    fi
fi
PATCH
        write_inplace "$SIM_WORK" "$SIM_TARGET"
        ANY_CHANGED=1
        echo "SIM_UICC: staged"
    fi
fi

if [ -n "$BOOT_TARGET" ] && [ "$DO_HOTSPOT" -eq 1 ]; then
    if grep -q 'override_tether_enable_bpf_offload' "$BOOT_TARGET" && grep -q -- '--dport 53 -j DNAT' "$BOOT_TARGET"; then
        echo "HOTSPOT_TETHER: already patched"
    else
        cp -a "$BOOT_TARGET" "$BOOT_WORK"
        awk '
        function emit_block() {
            print ""
            print "# Samsung A23 hotspot/tethering fixes:"
            print "# 1) QTI ipacm (swlan0=UNKNOWN) and 4.19 offload.o (BpfCoordinator ENOENT) break forwarding."
            print "# 2) TetheringNext passes usingLegacyDnsProxy=false (no dnsmasq) while IpServer sends"
            print "#    gateway IP as DHCP DNS; DNAT downstream port 53 to 8.8.8.8."
            print "if getprop ro.vendor.build.fingerprint | grep -qiE '\''^samsung/a23'\''; then"
            print "    if [ \"$(settings get global tether_offload_disabled)\" != \"1\" ]; then"
            print "        settings put global tether_offload_disabled 1"
            print "    fi"
            print "    if [ \"$(device_config get connectivity override_tether_enable_bpf_offload)\" != \"false\" ]; then"
            print "        device_config put connectivity override_tether_enable_bpf_offload false"
            print "    fi"
            print "    if [ \"$(device_config get tethering override_tether_enable_bpf_offload)\" != \"false\" ]; then"
            print "        device_config put tethering override_tether_enable_bpf_offload false"
            print "    fi"
            print "    nat_chain=\"PREROUTING\""
            print "    if /system/bin/iptables -w 5 -t nat -C PREROUTING -j oem_nat_pre >/dev/null 2>&1; then"
            print "        nat_chain=\"oem_nat_pre\""
            print "    fi"
            print "    for iface in swlan+ rndis+ usb+ ncm+ bt-pan; do"
            print "        for proto in udp tcp; do"
            print "            if ! /system/bin/iptables -w 5 -t nat -C \"$nat_chain\" -i \"$iface\" -p \"$proto\" --dport 53 -j DNAT --to-destination 8.8.8.8:53 2>/dev/null; then"
            print "                /system/bin/iptables -w 5 -t nat -A \"$nat_chain\" -i \"$iface\" -p \"$proto\" --dport 53 -j DNAT --to-destination 8.8.8.8:53"
            print "            fi"
            print "        done"
            print "    done"
            print "    log -t phh-on-boot \"Applied A23 tethering offload and DNS DNAT fixes (chain=$nat_chain)\""
            print "fi"
        }
        {
            if (!done && index($0, "#Clear looping services") > 0) {
                emit_block()
                print ""
                done = 1
            }
            print $0
        }
        END {
            if (!done) emit_block()
        }
        ' "$BOOT_WORK" > "$NEXT"
        mv "$NEXT" "$BOOT_WORK"
        write_inplace "$BOOT_WORK" "$BOOT_TARGET"
        ANY_CHANGED=1
        echo "HOTSPOT_TETHER: staged"
    fi
fi

if [ "$ANY_CHANGED" -eq 1 ]; then
    sync
    echo "PATCHED_GSI_OK"
else
    echo "NO_CHANGES_NEEDED"
fi

if [ -n "$SYS_MOUNT" ]; then
    mount -o remount,ro "$SYS_MOUNT" 2>/dev/null || true
fi
