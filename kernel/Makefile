obj-m += scthrottle.o

scthrottle-objs := \
  src/scth_main.o \
  src/scth_dev.o  \
  src/scth_ioctl.o \
  src/scth_cfg.o \
  src/scth_hook.o

ccflags-y += -I$(src)/../include -I$(src)/include
ccflags-y += -fcf-protection=branch