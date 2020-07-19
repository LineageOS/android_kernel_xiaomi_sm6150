ccflags-y := -I$(src) -Wall -Werror
IO_MODULE := msm

# Need to add include paths:
LINUXINCLUDE += \
             -I$(srctree)techpack/audio/include/dsp
             -I$(srctree)techpack/audio/include/uapi
             -I$(srctree)techpack/aduio/include

obj-y += elliptic.o
obj-y += elliptic_mixer_controls.o
obj-y += io_modules/$(IO_MODULE)/elliptic_data_$(IO_MODULE)_io.o
obj-y += io_modules/userspace/elliptic_data_userspace_io.o
obj-y += io_modules/userspace/elliptic_data_userspace_ctrl.o
obj-y += elliptic_sysfs.o
