# Samsung Galaxy A23 4G (`SM-A235M` / `a23ub`) — Kernel `4.19.157-perf` + KernelSU-Next + GSI Fixes

Repositorio base de la **Fase 1** para el Samsung Galaxy A23 4G (`SM-A235M`, SoC Qualcomm `SM6225` / `SDM680` / `Khaje`): kernel `4.19.157-perf` limpio de protecciones Samsung/Knox, compatible a nivel KABI/`MODVERSIONS` con los módulos binarios del vendor stock (Android 11 / `RP1A`, VNDK 30), adaptado para ejecutar GSIs **Android 16 (LineageOS 23.2 PHH)** y con **KernelSU-Next (`v3.2.0-legacy`)** integrado mediante hooks manuales.

---

## 1. Estado actual verificado (Fase 1 completada)

- **Kernel:** `Linux 4.19.157-perf #1 SMP PREEMPT aarch64` ([`a23_eur_open_defconfig`](SM-A235M_LA_14_Opensource/Kernel/arch/arm64/configs/a23_eur_open_defconfig))
- **Seguridad Samsung / Knox desactivada:** `UH`, `RKP`, `KDP`, `KDP_CRED`, `KDP_NS`, `KNOX_NCM`, `SECURITY_DEFEX`, `DEFEX_KERNEL_ONLY`, `HDM`, `PROCA`, `GAF`/`GAF_V6` y `FIVE` apagados vía [`knox_strip_defconfig.sh`](knox_strip_defconfig.sh).
- **Módulos stock de vendor operativos (`KABI` preservada):** `wlan` (`qca_cld3_wlan.ko`, `8,265,728` bytes en `lsmod`) carga sin errores de firma (`CONFIG_MODULE_SIG=n`) ni cambios de CRC en `MODVERSIONS` (`15c906bef`, `e01c08b9a`).
- **Backlight / brillo extendido en GSI:** [`a3c66848b`](SM-A235M_LA_14_Opensource/Kernel/drivers/gpu/drm/msm/samsung/ss_dsi_panel_sysfs.c) expone `max_brightness` (`306`) en `/sys/class/backlight/panel`.
- **Root en kernel y `adb shell`:** **KernelSU-Next `v3.2.0-legacy` (`KSU_VERSION=33129`, `Hook mode: Manual`)** verificado en hardware real (`uid=0(root) gid=0(root) context=u:r:ksu:s0`, SELinux `Enforcing`, Wi-Fi y RIL/telefonía activos).

---

## 2. Estructura del repositorio

| Ruta | Contenido |
|---|---|
| [`SM-A235M_LA_14_Opensource/Kernel/`](SM-A235M_LA_14_Opensource/Kernel) | Código fuente del kernel `4.19.157-perf` con parches KABI, display, defconfig y KernelSU-Next en [`drivers/kernelsu/`](SM-A235M_LA_14_Opensource/Kernel/drivers/kernelsu) |
| [`knox_strip_defconfig.sh`](knox_strip_defconfig.sh) | Script idempotente que desactiva las protecciones Knox/UH/RKP/KDP/DEFEX/HDM/PROCA/GAF/FIVE en el defconfig |
| [`bootimg_stock/`](bootimg_stock) | `ramdisk` y `dtb` stock extraídos para reempaquetar `boot.img` (`header_version 2`, `SRPUL29A015`) |
| [`tools/`](tools) | `mkbootimg` / `unpack_bootimg` y `make-4.2.1.tar.gz` usados por CI |
| [`.github/workflows/build.yml`](.github/workflows/build.yml) | Compila `Image.gz` con GNU Make 4.2.1 + `aarch64-linux-gnu-gcc` + `ccache` y genera `output/boot.img` (`a23-boot-img`) |
| [`gsi-fixes/`](gsi-fixes/README.md) | Script TWRP [`patch_rw_system_gsi.sh`](gsi-fixes/patch_rw_system_gsi.sh) y shim nativo [`rild_signal_fix.c`](gsi-fixes/rild-signal-fix/rild_signal_fix.c) para GSIs Android 16 (documentación completa en [`gsi-fixes/README.md`](gsi-fixes/README.md)) |
| [`.github/workflows/build-signal-fix.yml`](.github/workflows/build-signal-fix.yml) | Compila el binario `rild` arm64 (Android NDK API 30) para barras de señal (`a23-rild-signal-fix`) |

---

## 3. Integración de KernelSU-Next (`v3.2.0-legacy`, Hooks Manuales 4.19)

Implementado preservando estrictamente la KABI de los módulos stock (sin `CONFIG_KPROBES`, sin `SUSFS` y sin agregar campos que rompan `task_struct`/`cred`/`vfsmount`/`inode`/`file`):

- **Vendorizado:** `rifsxd/KernelSU-Next` tag `v3.2.0-legacy` (`33129`) en [`drivers/kernelsu/`](SM-A235M_LA_14_Opensource/Kernel/drivers/kernelsu), conectado en [`drivers/Kconfig`](SM-A235M_LA_14_Opensource/Kernel/drivers/Kconfig) y [`drivers/Makefile`](SM-A235M_LA_14_Opensource/Kernel/drivers/Makefile).
- **Configuración en [`a23_eur_open_defconfig`](SM-A235M_LA_14_Opensource/Kernel/arch/arm64/configs/a23_eur_open_defconfig):**
  - `CONFIG_KSU=y`
  - `CONFIG_KSU_MANUAL_HOOK=y`
  - `CONFIG_SECURITY_WRITABLE_HOOKS=y` *(obligatorio con `CONFIG_STRICT_KERNEL_RWX=y` para registrar los LSM hooks de KernelSU sin panic en `module_init`)*
  - `# CONFIG_KSU_KPROBES_HOOK is not set`
- **7 hooks manuales (`#ifdef CONFIG_KSU_MANUAL_HOOK`):**
  1. [`kernel/reboot.c`](SM-A235M_LA_14_Opensource/Kernel/kernel/reboot.c): `ksu_handle_sys_reboot` antes de `!ns_capable(pid_ns->user_ns, CAP_SYS_BOOT)`.
  2. [`fs/exec.c`](SM-A235M_LA_14_Opensource/Kernel/fs/exec.c): `ksu_handle_execveat` en `do_execveat_common`.
  3. [`fs/open.c`](SM-A235M_LA_14_Opensource/Kernel/fs/open.c): `ksu_handle_faccessat` en `do_faccessat`.
  4. [`fs/read_write.c`](SM-A235M_LA_14_Opensource/Kernel/fs/read_write.c): `ksu_handle_vfs_read` en `vfs_read` (desactivado tras leer `init.rc` junto con `ksu_vfs_read_hook = false` en [`ksud_integration.c`](SM-A235M_LA_14_Opensource/Kernel/drivers/kernelsu/runtime/ksud_integration.c)).
  5. [`fs/stat.c`](SM-A235M_LA_14_Opensource/Kernel/fs/stat.c): `ksu_handle_stat` en `vfs_statx`, `ksu_handle_newfstat_ret` en `newfstat` y `ksu_handle_fstat64_ret` en `fstat64`.
  6. [`drivers/input/input.c`](SM-A235M_LA_14_Opensource/Kernel/drivers/input/input.c): `ksu_handle_input_handle_event` en `input_event`.
  7. [`security/selinux/avc.c`](SM-A235M_LA_14_Opensource/Kernel/security/selinux/avc.c): `ksu_handle_slow_avc_audit` en `slow_avc_audit`.
- **Backports 4.19 limpios (sin parches `sed -i` en compilación):**
  - `can_umount()` / `path_umount()` en [`fs/namespace.c`](SM-A235M_LA_14_Opensource/Kernel/fs/namespace.c) y [`fs/internal.h`](SM-A235M_LA_14_Opensource/Kernel/fs/internal.h).
  - `atomic_t filter_count` en el relleno de alineación de `struct seccomp` en [`include/linux/seccomp.h`](SM-A235M_LA_14_Opensource/Kernel/include/linux/seccomp.h) (sin alterar `sizeof(struct seccomp)`).
  - Helpers `selinux_inode()` / `selinux_cred()` en [`security/selinux/include/objsec.h`](SM-A235M_LA_14_Opensource/Kernel/security/selinux/include/objsec.h).
  - Fallback `strscpy_pad` -> `strscpy` + `memset` para Linux `4.19.157` en [`drivers/kernelsu/compat/kernel_compat.h`](SM-A235M_LA_14_Opensource/Kernel/drivers/kernelsu/compat/kernel_compat.h).

---

## 4. Reglas importantes de uso (`adb shell su` vs `adb root`)

1. **NO ejecutar `adb root` dentro de Android:** en este dispositivo el controlador USB `4e00000.dwc3` pierde la sesión ADB al reiniciar `adbd` como root.
2. **Usar `su` mediante KernelSU-Next:**
   - Instalar el Manager exacto **`KernelSU_Next_v3.2.0_33129-release.apk`** (no actualizar a `v3.4.x`, que pide kernel/ABI `33355` y oculta la pestaña Superuser).
   - En **Superuser → ⋮ → Mostrar apps del sistema → Shell (`com.android.shell`, UID 2000)**, activar el permiso de root.
   - Probar en PC:
     ```sh
     adb shell "su -c id"
     # uid=0(root) gid=0(root) groups=0(root) context=u:r:ksu:s0
     ```

---

## 5. Historial de commits relevantes (`main`)

- `9732552b4`: `kernelsu: integrate KernelSU-Next v3.2.0-legacy with manual 4.19 hooks`
- `14777c35e`..`70f870234`: `gsi-fixes` — shim nativo de `rild` (`libril_sem` ID `2401036`/`2401009`), fix SMSC SMS y flujo NDK CI
- `f775f7f25`: `gsi-fixes` — fix hotspot Wi-Fi/datos (`tether_offload_disabled`, BPF offload y DNAT puerto 53)
- `d36547b30`: `gsi-fixes` — script TWRP inicial para almacenamiento/Codec2, brillo y recuperación SIM UICC
- `a3c66848b`: `display: expose panel max_brightness on /sys/class/backlight/panel`
- `e01c08b9a`: preservación KABI / `MODVERSIONS` para módulos stock
- `65646b215`: desactivación de `CONFIG_MODULE_SIG` para `qca_cld3_wlan.ko` + caché ccache en CI
- `15c906bef`: fix de parsing `__packed` en `genksyms` para CRCs `MODVERSIONS`
- `a7898426d`: importación inicial del kernel `SM-A235M` `4.19.157` limpio de Knox

---

## 6. Guía para Fase 2: Estudio del dispositivo con `su` para portear a Kernel 6.6

Con acceso `root` estable desde `adb shell` y sin alterar la conexión USB, esta es la lista base de recolección para diseñar el port a **ACK / Linux 6.6**:

```sh
# 1. Boot, cmdline, DTB real y mapa de hardware/memoria
adb shell "su -c 'cat /proc/cmdline; echo ---; cat /proc/iomem; echo ---; ls -la /sys/firmware/devicetree/base/'"

# 2. Volcar DTB vivo y descompilarlo (en PC con dtc)
adb shell "su -c 'cat /sys/firmware/fdt > /sdcard/live_fdt.dtb'"
adb pull /sdcard/live_fdt.dtb .

# 3. Módulos del kernel, firmwares y árboles de dispositivos activos
adb shell "su -c 'lsmod; echo ---; ls -la /vendor/lib/modules/; echo ---; ls -la /vendor/firmware_mnt/image/'"

# 4. Buses, drivers enlazados y nodos clave (USB dwc3, DSI panel, touch, cargador, sensores, remoteproc)
adb shell "su -c 'find /sys/bus/platform/devices -maxdepth 1; echo ---; ls -la /sys/class/backlight/ /sys/class/power_supply/ /sys/class/drm/'"

# 5. Logs completos de arranque del kernel 4.19 funcional (referencia de secuencias de probe)
adb shell "su -c 'dmesg > /sdcard/dmesg_4_19_working.txt'"
adb pull /sdcard/dmesg_4_19_working.txt .

# 6. Manifiesto VINTF, propiedades del vendor e init scripts (para compatibilidad ABI/userland con 6.6)
adb shell "su -c 'getprop > /sdcard/props_a23.txt; cp /vendor/etc/vintf/manifest.xml /sdcard/vendor_manifest.xml'"
adb pull /sdcard/props_a23.txt .
adb pull /sdcard/vendor_manifest.xml .
```

### Puntos críticos al portear a Kernel 6.6
1. **Árbol de dispositivos (`dts`/`dtb`) y memoria:** comparar `/sys/firmware/fdt` y `/proc/iomem` con los nodos de `SM6225`/`Khaje` en `6.6` (revisar `reserved-memory`, `remoteproc` del módem/ADSP/CDSP, `4e00000.dwc3` y `mdss_dsi`).
2. **Controladores específicos Samsung (`SM-A235M`):** panel DSI (`ss_dsi_panel` + `ss_blic_ctrl` + `max_brightness=306`), IC de carga/USB-C/MUIC, touchscreen y sensores Samsung en `drivers/` que no existen en ACK 6.6 mainline.
3. **Compatibilidad con vendor Android 11 (`VNDK 30`):** revisar en `dmesg` cómo `wlan.ko`, `rild`/QMI/IPA, Audio (`ADSP`) y Camera interactúan con el kernel actual (`netfilter`/`xt_*`, `binder`, `ashmem`/`memfd`, `seccomp`, `selinux`).
