# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the kernel security code
#

obj-$(CONFIG_KEYS)			+= keys/
subdir-$(CONFIG_SECURITY_SELINUX)	+= selinux
subdir-$(CONFIG_SECURITY_SMACK)		+= smack
subdir-$(CONFIG_SECURITY_TOMOYO)        += tomoyo
subdir-$(CONFIG_SECURITY_APPARMOR)	+= apparmor
subdir-$(CONFIG_SECURITY_YAMA)		+= yama
subdir-$(CONFIG_SECURITY_LOADPIN)	+= loadpin
subdir-$(CONFIG_SECURITY_DEFEX)		+= samsung/defex_lsm

# always enable default capabilities
obj-y					+= commoncap.o
obj-$(CONFIG_MMU)			+= min_addr.o

# Object file lists
obj-$(CONFIG_SECURITY)			+= security.o
obj-$(CONFIG_SECURITYFS)		+= inode.o
obj-$(CONFIG_SECURITY_SELINUX)		+= selinux/
obj-$(CONFIG_SECURITY_SMACK)		+= smack/
obj-$(CONFIG_AUDIT)			+= lsm_audit.o
obj-$(CONFIG_SECURITY_TOMOYO)		+= tomoyo/
obj-$(CONFIG_SECURITY_APPARMOR)		+= apparmor/
obj-$(CONFIG_SECURITY_YAMA)		+= yama/
obj-$(CONFIG_SECURITY_LOADPIN)		+= loadpin/
obj-$(CONFIG_SECURITY_DEFEX)		+= samsung/defex_lsm/built-in.a
obj-$(CONFIG_CGROUP_DEVICE)		+= device_cgroup.o

# Object integrity file lists
subdir-$(CONFIG_INTEGRITY)		+= integrity
obj-$(CONFIG_INTEGRITY)			+= integrity/

# HDM driver
obj-$(CONFIG_HDM)			+= hdm/
# Knox-Stripped: las siguientes líneas fueron inyectadas por el build de Samsung
# ("ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT") forzando el descenso incondicional.
# Ahora se gatean por su config. defex_lsm ya está gateado arriba
# (subdir-/obj-$(CONFIG_SECURITY_DEFEX)).
obj-$(CONFIG_PROCA)		+= samsung/proca/
obj-$(CONFIG_PROCA)		+= samsung/proca/gaf/
obj-$(CONFIG_FIVE)		+= samsung/five/
