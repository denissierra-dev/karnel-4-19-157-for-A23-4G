#!/usr/bin/env bash
#
# knox_strip_defconfig.sh — Samsung SM-A235M (Khaje / SDM680) "Knox-Stripped"
#
# Desactiva la pila de seguridad de Samsung en el defconfig de la placa,
# dejando las directivas estrictamente como:  # CONFIG_NOMBRE is not set
#
# Uso:
#   ./knox_strip_defconfig.sh [ruta/al/defconfig]
#
# Por defecto apunta al defconfig real de A23 (EUR open):
#   arch/arm64/configs/a23_eur_open_defconfig
#
# NOTA: los nombres pedidos (SEC_DEFEX, RKP_KDP, TZ_ICCC, KNOX_KAP) son de
# Exynos. En este SoC Qcom los símbolos reales son los de la lista de abajo.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${SCRIPT_DIR}/SM-A235M_LA_14_Opensource/Kernel"
DEFCONFIG="${1:-${KERNEL_DIR}/arch/arm64/configs/a23_eur_open_defconfig}"

if [[ ! -f "$DEFCONFIG" ]]; then
    echo "ERROR: no existe $DEFCONFIG" >&2
    exit 1
fi

cp -a "$DEFCONFIG" "${DEFCONFIG}.bak"

#
# Símbolos bool/tristate a desactivar (-> "# CONFIG_X is not set")
#   - UH/RKP/KDP/KDP_* : micro-hypervisor + Realtime/Kernel Data Protection
#                        (sustituyen a tu "RKP_KDP")
#   - KNOX_NCM         : (sustituye a tu "KNOX_KAP")
#   - PROCA*           : Process Authenticator
#   - FIVE             : Task Integrity Verifier (ya =n, se deja por idempotencia)
#   - SECURITY_DEFEX   : Defex LSM (sustituye a tu "SEC_DEFEX")
#   - HDM              : único caller SMC sin guard de config; llama uh_call()
#                        incondicionalmente si ARCH_QCOM está activo.
#   - GAF / GAF_V6     : Google Android Framework verifier (hermano de PROCA).
#
declare -a BOOL_CONFIGS=(
    CONFIG_UH
    CONFIG_RKP
    CONFIG_KDP
    CONFIG_KDP_CRED
    CONFIG_KDP_NS
    CONFIG_KNOX_NCM
    CONFIG_PROCA
    CONFIG_PROCA_S_OS
    CONFIG_PROCA_CERTIFICATES_XATTR
    CONFIG_FIVE
    CONFIG_SECURITY_DEFEX
    CONFIG_DEFEX_KERNEL_ONLY
    CONFIG_HDM
    CONFIG_GAF
    CONFIG_GAF_V6
)

#
# Símbolos de cadena (string) que dependen de los anteriores. Al apagar PROCA
# quedan inertes, pero se comentan para dejar el .config limpio.
#
declare -a STR_CONFIGS=(
    CONFIG_PROCA_CERT_ENG
    CONFIG_PROCA_CERT_USER
    CONFIG_FIVE_CERT_USER
)

for cfg in "${BOOL_CONFIGS[@]}"; do
    sed -i -E \
        -e "s|^${cfg}=y\$|# ${cfg} is not set|" \
        -e "s|^${cfg}=m\$|# ${cfg} is not set|" \
        -e "s|^${cfg}=n\$|# ${cfg} is not set|" \
        "$DEFCONFIG"
done

for cfg in "${STR_CONFIGS[@]}"; do
    # string: CONFIG_X="valor" -> se comenta (no admite "is not set")
    sed -i -E "s|^${cfg}=|# ${cfg}=|" "$DEFCONFIG"
done

echo "== Resultado (verificación) =="
for cfg in "${BOOL_CONFIGS[@]}" "${STR_CONFIGS[@]}"; do
    grep -nE "^#? ?${cfg}(=| is not set)" "$DEFCONFIG" || true
done

echo
echo "Backup guardado en ${DEFCONFIG}.bak"
echo "Reconstruye el .config con:"
echo "  make O=out ARCH=arm64 CROSS_COMPILE=... a23_eur_open_defconfig"
