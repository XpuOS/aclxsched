.DEFAULT_GOAL      := build
WORK_PATH          := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

CMAKE              ?= cmake
CMAKE_ARGS         ?=
BUILD_TYPE         ?= Release
VERBOSE            ?= OFF
SHIM_SOFTLINK      ?= ON
BUILD_TEST         ?= OFF
PLATFORM           ?= NONE

BUILD_DIR          ?= $(WORK_PATH)/build
INSTALL_DIR        ?= $(WORK_PATH)/output
# Retain the old path variables as optional overrides.
BUILD_PATH         ?= $(abspath $(BUILD_DIR))
INSTALL_PATH       ?= $(abspath $(INSTALL_DIR))

XSCHED_ASCEND_XVMM ?= ON
ASCEND_ENABLE_ATB  ?= ON
CANN_ROOT          ?=
ATB_ROOT           ?=

ifeq ($(OS),Windows_NT)
CPU_CORES ?= $(NUMBER_OF_PROCESSORS)
CMAKE_OS_FLAGS ?= -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
else
CPU_CORES ?= $(shell getconf _NPROCESSORS_ONLN)
CMAKE_OS_FLAGS ?=
endif
JOBS ?= $(CPU_CORES)
SUPPORTED_PLATFORMS := $(notdir $(patsubst %/,%,$(dir $(wildcard $(WORK_PATH)/platforms/*/CMakeLists.txt))))

define uppercase
$(subst z,Z,$(subst y,Y,$(subst x,X,$(subst w,W,$(subst v,V,$(subst u,U,$(subst t,T,$(subst s,S,$(subst r,R,$(subst q,Q,$(subst p,P,$(subst o,O,$(subst n,N,$(subst m,M,$(subst l,L,$(subst k,K,$(subst j,J,$(subst i,I,$(subst h,H,$(subst g,G,$(subst f,F,$(subst e,E,$(subst d,D,$(subst c,C,$(subst b,B,$(subst a,A,$(1)))))))))))))))))))))))))))
endef

.PHONY: all configure build install clean $(SUPPORTED_PLATFORMS)
all: build

# Reconfigure on every invocation: command-line options must not be lost behind
# an existing CMakeCache.txt. Explicit OFF values clear previously enabled platforms.
configure:
	$(CMAKE) -S "$(WORK_PATH)" -B "$(BUILD_PATH)" $(CMAKE_OS_FLAGS) \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DCMAKE_VERBOSE_MAKEFILE="$(VERBOSE)" \
		-DCMAKE_INSTALL_PREFIX="$(abspath $(INSTALL_PATH))" \
		-DSHIM_SOFTLINK="$(SHIM_SOFTLINK)" -DBUILD_TEST="$(BUILD_TEST)" \
		-DXSCHED_ASCEND_XVMM="$(XSCHED_ASCEND_XVMM)" -DASCEND_ENABLE_ATB="$(ASCEND_ENABLE_ATB)" \
		$(if $(strip $(CANN_ROOT)),-DASCEND_CANN_ROOT:PATH="$(CANN_ROOT)") \
		$(if $(strip $(ATB_ROOT)),-DASCEND_ATB_ROOT:PATH="$(ATB_ROOT)") \
		$(foreach p,$(SUPPORTED_PLATFORMS),-DPLATFORM_$(call uppercase,$(p))=$(if $(filter $(p),$(PLATFORM)),ON,OFF)) \
		$(CMAKE_ARGS)

build: configure
	$(CMAKE) --build "$(BUILD_PATH)" --target install --parallel "$(JOBS)"
install: build

$(SUPPORTED_PLATFORMS):
	+$(MAKE) --no-print-directory build PLATFORM=$@

clean:
ifneq ($(wildcard $(BUILD_PATH)/CMakeCache.txt),)
	$(CMAKE) --build "$(BUILD_PATH)" --target clean
else
	@echo No configured build directory to clean: "$(BUILD_PATH)"
endif
