# OpenBOR multi-engine libretro core
#
#   make                       build for the host (Linux/BSD, macOS, Windows/MinGW)
#   make ENGINES="6391 7555"   choose the engine versions to include
#   make platform=win          cross/MinGW build
#   make DEBUG=1               unoptimised build with symbols
#   make SYSTEM_ZLIB=1 SYSTEM_LIBPNG=1 SYSTEM_VORBIS=1
#                              link against system libraries instead of deps/
#   make standalone            the same engines as a program with an SDL2
#                              host (no frontend needed): openbor / openbor.exe,
#                              engine chosen with --engine (standalone/main.c);
#                              SDL2_CONFIG=<path to sdl2-config> for another SDL2
#                              (Windows: linked statically, one .exe)

CORE_DIR := .
.DEFAULT_GOAL := all
TARGET_NAME := openbor

ifeq ($(platform),)
   platform = unix
   ifeq ($(shell uname -s),)
      platform = win
   else ifneq ($(findstring MINGW,$(shell uname -s)),)
      platform = win
   else ifneq ($(findstring MSYS,$(shell uname -s)),)
      platform = win
   else ifneq ($(findstring Darwin,$(shell uname -s)),)
      platform = osx
   endif
endif

GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_VERSION),)
   DEFINES += -DGIT_VERSION=\"\ $(GIT_VERSION)\"
endif

CC ?= gcc
OBJCOPY ?= objcopy
LIBS := -lm
SYM_PREFIX :=

ifeq ($(platform), unix)
   TARGET  := $(TARGET_NAME)_libretro.so
   fpic    := -fPIC
   SHARED  := -shared -Wl,--version-script=$(CORE_DIR)/link.T -Wl,--no-undefined
   DEFINES += -DLINUX
else ifeq ($(platform), osx)
   TARGET  := $(TARGET_NAME)_libretro.dylib
   fpic    := -fPIC
   SHARED  := -dynamiclib
   DEFINES += -DLINUX -DDARWIN
   SYM_PREFIX := _
   ifneq ($(arch),)
      CFLAGS  += -arch $(arch)
      LDFLAGS += -arch $(arch)
   endif
else ifeq ($(platform), win)
   TARGET  := $(TARGET_NAME)_libretro.dll
   SHARED  := -shared -static-libgcc -Wl,--version-script=$(CORE_DIR)/link.T -Wl,--no-undefined
   DEFINES += -DWIN
   LIBS    += -lpsapi
else
   $(error Unsupported platform '$(platform)')
endif

include Makefile.common

# WebM playback (engine 8025_webm) decodes in threads (Win32 threads on Windows)
ifneq ($(filter 8025_webm,$(ENGINES)),)
   ifneq ($(platform), win)
      LIBS += -lpthread
   endif
endif

ifeq ($(SYSTEM_ZLIB), 1)
   LIBS += -lz
endif
ifeq ($(SYSTEM_LIBPNG), 1)
   LIBS += -lpng
endif
ifeq ($(SYSTEM_VORBIS), 1)
   LIBS += -lvorbisfile -lvorbis -logg
endif

DEFINES += -D__LIBRETRO__ -DPNG_NO_CONFIG_H

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g -DDEBUG
else
   CFLAGS += -O2 -DNDEBUG
endif

# -MMD -MP: header dependencies (obj/**/*.d)
CFLAGS += -MMD -MP
CFLAGS += -std=gnu99 -fsigned-char -fno-strict-aliasing -Wall \
          -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
          $(fpic) $(DEFINES) $(INCFLAGS)

# Objects go to obj/<platform>/; each engine has its own subdirectory, as
# the same sources can be compiled for two engines (8025 and 8025_webm).
OBJDIR := obj/$(platform)$(if $(filter 1,$(DEBUG)),-debug)

OBJECTS := $(patsubst %.c,$(OBJDIR)/%.o,$(SOURCES_C))

# ---------------------------------------------------------------------------
# Each engine version is compiled with its own include paths, partially
# linked into one object and every global symbol except its ob_engine_<ver>
# accessor is made local, so several engines can coexist in the core.
# ---------------------------------------------------------------------------
define ENGINE_RULES
ENGINE_$(1)_OBJECTS := $$(patsubst %.c,$(OBJDIR)/engine_$(1)/%.o,$$(ENGINE_$(1)_SOURCES))
ENGINE_OBJECTS += $(OBJDIR)/engines/ob_engine_$(1).o
CFLAGS += -DHAVE_ENGINE_$(1)

$$(ENGINE_$(1)_OBJECTS): $(OBJDIR)/engine_$(1)/%.o: %.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) $$(ENGINE_$(1)_FLAGS) -DLIBRETRO -DOB_ENGINE_ID=$(1) '-DOB_ENGINE_NAME="$$(ENGINE_$(1)_NAME)"' -c -o $$@ $$<

$(OBJDIR)/engines/ob_engine_$(1).o: $$(ENGINE_$(1)_OBJECTS)
	@mkdir -p $$(dir $$@)
ifeq ($(platform), osx)
	ld -r $(LDFLAGS) -exported_symbol $(SYM_PREFIX)ob_engine_$(1) -o $$@ $$^
else
	$$(CC) $(fpic) -nostdlib -r -Wl,-d -o $$@.tmp $$^
	$$(OBJCOPY) --keep-global-symbol=$(SYM_PREFIX)ob_engine_$(1) $$@.tmp $$@
	rm -f $$@.tmp
endif
endef

ENGINE_OBJECTS :=
$(foreach e,$(ENGINES),$(eval $(call ENGINE_RULES,$(e))))

all: $(TARGET)

$(TARGET): $(OBJECTS) $(ENGINE_OBJECTS)
	$(CC) $(fpic) $(SHARED) $(LDFLAGS) -o $@ $(OBJECTS) $(ENGINE_OBJECTS) $(LIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJECTS:.o=.d) $(foreach e,$(ENGINES),$(ENGINE_$(e)_OBJECTS:.o=.d))

# ---------------------------------------------------------------------------
# Standalone program: the core's objects + the SDL2 host (standalone/main.c)
# ---------------------------------------------------------------------------
SDL2_CONFIG ?= sdl2-config
ifeq ($(platform), win)
   STANDALONE   := $(TARGET_NAME).exe
   # one .exe: SDL2 and the C runtime linked in; a console program (the
   # command line options and the messages)
   SDL_LIBS      = $(shell $(SDL2_CONFIG) --static-libs) -static -mconsole
else
   STANDALONE   := $(TARGET_NAME)
   SDL_LIBS      = $(shell $(SDL2_CONFIG) --libs)
endif
SDL_CFLAGS = $(shell $(SDL2_CONFIG) --cflags)

$(OBJDIR)/standalone/main.o: standalone/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

standalone: $(STANDALONE)

$(STANDALONE): $(OBJECTS) $(ENGINE_OBJECTS) $(OBJDIR)/standalone/main.o
	$(CC) $(LDFLAGS) -o $@ $(OBJDIR)/standalone/main.o $(OBJECTS) $(ENGINE_OBJECTS) $(SDL_LIBS) $(LIBS)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(STANDALONE)

.PHONY: all clean standalone
