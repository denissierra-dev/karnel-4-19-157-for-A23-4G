#include <linux/linkage.h>
#include <linux/uh.h>

/*
 * Knox-Stripped: uh_call() es el embudo único de todas las SMC propietarias al
 * micro-hipervisor de Samsung (RKP, KDP, HDM, CFP, INIT). El cuerpo original
 * emitía `smc #0x0` hacia EL3/TrustZone y hacía spin mientras el hipervisor
 * devolvía "reintentar" (x0 == 1).
 *
 * Aquí se neutraliza: el símbolo sigue definido (ningún caller produce
 * undefined reference) y devuelve 0 (éxito) sin volver a trapear al hipervisor,
 * permitiendo que el arranque continúe con RKP/KDP/DEFEX/PROCA/FIVE/KNOX
 * descompilados.
 */
ENTRY(uh_call)
	mov	x0, #0		/* 0 == success */
	ret
ENDPROC(uh_call)
