GVT_DIR := gvt
GVT_SOURCE := gvt.o aperture_gm.o handlers.o vgpu.o trace_points.o firmware.o \
	interrupt.o gtt.o cfg_space.o opregion.o mmio.o display.o edid.o \
	execlist.o scheduler.o sched_policy.o render.o cmd_parser.o

ccflags-y				+= -I$(src) -I$(src)/$(GVT_DIR) -Wall
i915-y					+= $(addprefix $(GVT_DIR)/, $(GVT_SOURCE))
obj-$(CONFIG_DRM_I915_GVT_KVMGT)	+= $(GVT_DIR)/kvmgt.o
obj-$(CONFIG_DRM_I915_GVT_XENGT)	+= $(GVT_DIR)/xengt.o
