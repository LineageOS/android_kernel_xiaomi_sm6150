# auto-detect subdirs
ifneq ($(CONFIG_ARCH_ATOLL), y)
ifeq ($(CONFIG_ARCH_SDM845), y)
include $(srctree)/techpack/audio/config/sdm845auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdm845auto.conf)
endif
ifeq ($(CONFIG_ARCH_SDM660), y)
include $(srctree)/techpack/audio/config/sdm660auto_static.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdm660auto_static.conf)
endif
ifeq ($(CONFIG_ARCH_SDM670), y)
include $(srctree)/techpack/audio/config/sdm670auto_static.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdm670auto_static.conf)
endif
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
include $(srctree)/techpack/audio/config/sdxpoorwillsauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdxpoorwillsauto.conf)
endif
ifeq ($(CONFIG_ARCH_SM6150), y)
include $(srctree)/techpack/audio/config/sm6150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm6150auto.conf)
endif
ifeq ($(CONFIG_ARCH_SDMMAGPIE), y)
include $(srctree)/techpack/audio/config/sm6150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm6150auto.conf)
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
include $(srctree)/techpack/audio/config/sm8150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm8150auto.conf)
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
include $(srctree)/techpack/audio/config/sm8150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm8150auto.conf)
endif
ifeq ($(CONFIG_MACH_XIAOMI_COURBET), y)
include $(srctree)/techpack/audio/config/xiaomi/courbetauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/xiaomi/courbetauto.conf)
endif
ifeq ($(CONFIG_MACH_XIAOMI_DAVINCI), y)
include $(srctree)/techpack/audio/config/xiaomi/davinciauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/xiaomi/davinciauto.conf)
endif
ifeq ($(CONFIG_MACH_XIAOMI_PHOENIX), y)
include $(srctree)/techpack/audio/config/xiaomi/phoenixauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/xiaomi/phoenixauto.conf)
endif
ifeq ($(CONFIG_MACH_XIAOMI_SWEET), y)
include $(srctree)/techpack/audio/config/xiaomi/sweetauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/xiaomi/sweetauto.conf)
endif
ifeq ($(CONFIG_MACH_XIAOMI_TOCO), y)
include $(srctree)/techpack/audio/config/xiaomi/tocoauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/xiaomi/tocoauto.conf)
endif
ifeq ($(CONFIG_MACH_XIAOMI_TUCANA), y)
include $(srctree)/techpack/audio/config/xiaomi/tucanaauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/xiaomi/tucanaauto.conf)
endif

# Use USERINCLUDE when you must reference the UAPI directories only.
USERINCLUDE     += \
                -I$(srctree)/techpack/audio/include/uapi \

# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    += \
                -I$(srctree)/techpack/audio/include/uapi \
                -I$(srctree)/techpack/audio/include

ifeq ($(CONFIG_US_PROXIMITY), y)
LINUXINCLUDE    += \
                -I$(srctree)/techpack/audio/include/mius
endif

ifeq ($(CONFIG_ARCH_SDM845), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdm845autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDM660), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdm660autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDM670), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdm670autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdxpoorwillsautoconf.h
endif
ifeq ($(CONFIG_ARCH_SM6150), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm6150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDMMAGPIE), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm6150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_MACH_XIAOMI_COURBET), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/xiaomi/courbetautoconf.h
endif
ifeq ($(CONFIG_MACH_XIAOMI_DAVINCI), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/xiaomi/davinciautoconf.h
endif
ifeq ($(CONFIG_MACH_XIAOMI_PHOENIX), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/xiaomi/phoenixautoconf.h
endif
ifeq ($(CONFIG_MACH_XIAOMI_SWEET), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/xiaomi/sweetautoconf.h
endif
ifeq ($(CONFIG_MACH_XIAOMI_TOCO), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/xiaomi/tocoautoconf.h
endif
ifeq ($(CONFIG_MACH_XIAOMI_TUCANA), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/xiaomi/tucanaautoconf.h
endif

obj-y += soc/
obj-y += dsp/
obj-y += ipc/
obj-y += asoc/
else
obj-y += 4.0/
endif
