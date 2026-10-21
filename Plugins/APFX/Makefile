# APFX.ofx.bundle: the APFX nodes for SOLVFX.
#   make OFX_ROOT=<path to libs/OpenFX> BUNDLE_ARCH=Win64|Linux-x86-64 [CXX=...]
# Builds APFX.ofx with the OpenFX Support library and creates APFX.ofx.bundle here.

OFX_ROOT ?= ../../libs/OpenFX
CXX ?= g++
OPTFLAG ?= -O3
UNAME_S := $(shell uname -s)

ifeq ($(OS),Windows_NT)
  BUNDLE_ARCH ?= Win64
  LDFLAGS_OS = -shared -static-libgcc -static-libstdc++ -Wl,--build-id
  CXXFLAGS_OS = -DWIN32 -D_WINDOWS -DNOMINMAX
else ifneq (,$(findstring MINGW,$(UNAME_S)))
  BUNDLE_ARCH ?= Win64
  LDFLAGS_OS = -shared -static-libgcc -static-libstdc++ -Wl,--build-id
  CXXFLAGS_OS = -DWIN32 -D_WINDOWS -DNOMINMAX
else
  BUNDLE_ARCH ?= Linux-x86-64
  LDFLAGS_OS = -shared -Wl,-Bsymbolic -Wl,--version-script=$(OFX_ROOT)/Support/include/linuxSymbols
  CXXFLAGS_OS = -fPIC -fvisibility=hidden
endif

DEFS = -DOFX_EXTENSIONS_NUKE -DOFX_EXTENSIONS_NATRON -DOFX_EXTENSIONS_VEGAS -DOFX_SUPPORTS_PARAMETRIC \
       -DOFX_SUPPORTS_MULTITHREAD -DOFX_SUPPORTS_OPENGLRENDER -DOFX_SUPPORTS_DIALOG -DOFX_EXTENSIONS_RESOLVE
INCS = -I$(OFX_ROOT)/include -I$(OFX_ROOT)/Support/include -I$(OFX_ROOT)/Support/Plugins/include -INoise -IDefocus
CXXFLAGS += -std=c++11 $(OPTFLAG) $(CXXFLAGS_OS) $(DEFS) $(INCS) -Wall

SUPPORT_SRC = $(addprefix $(OFX_ROOT)/Support/Library/, ofxsCore.cpp ofxsImageEffect.cpp ofxsInteract.cpp ofxsLog.cpp \
              ofxsMultiThread.cpp ofxsParams.cpp ofxsProperty.cpp ofxsPropertyValidation.cpp)
SRC = Noise/APFXNoise.cpp Defocus/APFXDefocus.cpp

BUILD = build-$(BUNDLE_ARCH)
OBJS = $(addprefix $(BUILD)/, $(notdir $(SRC:.cpp=.o)) $(notdir $(SUPPORT_SRC:.cpp=.o)))

vpath %.cpp . Noise Defocus $(OFX_ROOT)/Support/Library

all: APFX.ofx.bundle

$(BUILD)/%.o: %.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/APFX.ofx: $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS_OS) $(LDFLAGS) -o $@

APFX.ofx.bundle: $(BUILD)/APFX.ofx Info.plist
	rm -rf APFX.ofx.bundle
	mkdir -p APFX.ofx.bundle/Contents/$(BUNDLE_ARCH) APFX.ofx.bundle/Contents/Resources
	cp $(BUILD)/APFX.ofx APFX.ofx.bundle/Contents/$(BUNDLE_ARCH)/
	cp Info.plist APFX.ofx.bundle/Contents/
	-cp Resources/*.png APFX.ofx.bundle/Contents/Resources/ 2>/dev/null || true

clean:
	rm -rf build-* APFX.ofx.bundle

.PHONY: all clean APFX.ofx.bundle
