# sloperinaGZ (source: src/ootgz): the native (in-game) build. See BUILDING.md.
#
#   make            build bin/<version>/subsdk9 and bin/<version>/<build id>.ips
#   make dist       stage install trees in dist/ (atmosphere/, lichii/, exefs/)
#   make clean
#
# Needs devkitA64 (DEVKITPRO set) and a host C compiler; ./docker-make runs this Makefile in
# the devkitpro/devkita64 image instead.

# the version: the VERSION file (the release) and git's commit, e.g. "0.1.0 (6d8cdba)"
PACKAGE_VERSION      ?= $(shell cat VERSION 2>/dev/null || echo 0) ($(shell git describe --tags --always --dirty 2>/dev/null || echo unknown))
DEVKITPRO            ?= /opt/devkitpro
TOOLS                 = $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CC                    = $(TOOLS)gcc
ELF2NSO               = $(DEVKITPRO)/tools/bin/elf2nso
HOSTCC               ?= gcc

VERSIONS              = v131072
PROGRAM_ID            = 0400486024DEC000
BUILD_ID-v131072      = E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348
VERSION_DEFINE-v131072 = Z64HD_V131072
SUBSDK                = subsdk9
NAME                  = sloperinaGZ

SRCDIR                = src/ootgz
HOSTDIR               = src/host
LDSCRIPT              = lib/ootgz.ld
OBJDIR                = obj
BINDIR                = bin
DISTDIR               = dist

# Extra -D defines: make OPTIONS=...
OPTIONS              ?=

ARCHFLAGS             = -march=armv8-a+crc+crypto -mtune=cortex-a78 -mtp=soft -fPIC
CFLAGS                = -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter $(ARCHFLAGS) \
                        -fvisibility=hidden -ffunction-sections -fdata-sections \
                        -fno-asynchronous-unwind-tables -fno-unwind-tables \
                        -fno-stack-protector -fno-plt -fno-common \
                        -DPACKAGE_VERSION='"$(PACKAGE_VERSION)"' $(addprefix -D,$(OPTIONS))
ASFLAGS               = $(ARCHFLAGS)
LDFLAGS               = -shared -nostdlib -nostartfiles -T $(LDSCRIPT) \
                        -Wl,--gc-sections -Wl,-z,text -Wl,-z,max-page-size=0x1000 \
                        -Wl,--hash-style=sysv -Wl,-Bsymbolic -Wl,--build-id=sha1 \
                        -Wl,-init=ootgz_module_init -Wl,--exclude-libs=ALL
LDLIBS                = -lgcc

CSRC                  = $(wildcard $(SRCDIR)/*.c)
SSRC                  = $(wildcard $(SRCDIR)/*.S)
HDRS                  = $(wildcard $(SRCDIR)/*.h)

all: $(foreach v,$(VERSIONS),$(BINDIR)/$(v)/$(SUBSDK) $(BINDIR)/$(v)/$(BUILD_ID-$(v)).ips)

# The about menu's build info: the version, the day it was built ("9th November, 2026") and the names in
# CONTRIBUTORS.md and THANKS.md
# ("- name" lines). Made every time; menus.c is rebuilt when it changes.
$(OBJDIR)/build_info.h: CONTRIBUTORS.md THANKS.md VERSION FORCE
	mkdir -p $(@D)
	d=$$(LC_ALL=C date +%-d); \
	case $$d in 11|12|13) o=th;; *1) o=st;; *2) o=nd;; *3) o=rd;; *) o=th;; esac; \
	{ printf '#define BUILD_WHEN "%s%s %s"\n' "$$d" "$$o" "$$(LC_ALL=C date +'%B, %Y')"; \
	  printf '#define BUILD_VERSION "%s"\n' "$(PACKAGE_VERSION)"; \
	  printf 'static const char *const contributors[] = {\n'; \
	  sed -n 's/^[-*][ \t]*\(.*[^ \t]\)[ \t]*$$/  "\1",/p' CONTRIBUTORS.md | tr -d '\r'; \
	  printf '};\nstatic const char *const thanks[] = {\n'; \
	  sed -n 's/^[-*][ \t]*\(.*[^ \t]\)[ \t]*$$/  "\1",/p' THANKS.md | tr -d '\r'; \
	  printf '};\n'; } > $@.tmp
	cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
FORCE:

define version_rules
$(OBJDIR)/$(1)/%.o: $(SRCDIR)/%.c $(HDRS) | $(OBJDIR)/$(1)
	$(CC) $(CFLAGS) -I$(OBJDIR) -DZ64HD_VERSION=$(VERSION_DEFINE-$(1)) -c $$< -o $$@
$(OBJDIR)/$(1)/menus.o: $(OBJDIR)/build_info.h
$(OBJDIR)/$(1)/%.o: $(SRCDIR)/%.S | $(OBJDIR)/$(1)
	$(CC) $(ASFLAGS) -c $$< -o $$@
$(BINDIR)/$(1)/ootgz.elf: $(patsubst $(SRCDIR)/%,$(OBJDIR)/$(1)/%.o,$(basename $(SSRC) $(CSRC))) $(LDSCRIPT) | $(BINDIR)/$(1)
	$(CC) $(ARCHFLAGS) $(LDFLAGS) -o $$@ $$(filter %.o,$$^) $(LDLIBS)
	@# a function we call but don't define links fine and only fails when the game loads the module:
	@# anything imported must be in lib/imports.txt (what main and sdk are known to export)
	@missing=$$$$($(TOOLS)readelf --dyn-syms -W $$@ | awk '$$$$7=="UND" && $$$$8!="" {print $$$$8}' | LC_ALL=C sort -u | LC_ALL=C comm -23 - lib/imports.txt); \
	if [ -n "$$$$missing" ]; then echo "error: not defined, and not a known import (lib/imports.txt):"; echo "$$$$missing"; rm -f $$@; exit 1; fi
$(BINDIR)/$(1)/$(SUBSDK): $(BINDIR)/$(1)/ootgz.elf
	$(ELF2NSO) $$< $$@
$(BINDIR)/$(1)/$(BUILD_ID-$(1)).ips: $(OBJDIR)/host/mkips | $(BINDIR)/$(1)
	$(OBJDIR)/host/mkips $$@
$(OBJDIR)/$(1) $(BINDIR)/$(1):
	mkdir -p $$@
endef
$(foreach v,$(VERSIONS),$(eval $(call version_rules,$(v))))

$(OBJDIR)/host/mkips: $(HOSTDIR)/mkips.c $(SRCDIR)/z64hd_v131072.h $(SRCDIR)/ootgz_version.h
	mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -o $@ $<

# Install trees. Atmosphère convention (also what a Switch 2 CFW with LayeredFS would need):
#   atmosphere/contents/<program id>/exefs/subsdk9
#   atmosphere/exefs_patches/$(NAME)/<build id>.ips
# Lichii / Eden: load/<program id>/ootgz/exefs/{subsdk9,<build id>.ips}
# exefs/: the two files for an offline patcher (add subsdk9 to exefs, apply the .ips to main).
dist: all
	rm -rf $(DISTDIR)
	$(foreach v,$(VERSIONS), \
	  mkdir -p $(DISTDIR)/atmosphere/contents/$(PROGRAM_ID)/exefs \
	           $(DISTDIR)/atmosphere/exefs_patches/$(NAME) \
	           $(DISTDIR)/lichii/load/$(PROGRAM_ID)/$(NAME)/exefs \
	           $(DISTDIR)/exefs/$(v) && \
	  cp $(BINDIR)/$(v)/$(SUBSDK) $(DISTDIR)/atmosphere/contents/$(PROGRAM_ID)/exefs/ && \
	  cp $(BINDIR)/$(v)/$(BUILD_ID-$(v)).ips $(DISTDIR)/atmosphere/exefs_patches/$(NAME)/ && \
	  cp $(BINDIR)/$(v)/$(SUBSDK) $(BINDIR)/$(v)/$(BUILD_ID-$(v)).ips \
	     $(DISTDIR)/lichii/load/$(PROGRAM_ID)/$(NAME)/exefs/ && \
	  cp $(BINDIR)/$(v)/$(SUBSDK) $(BINDIR)/$(v)/$(BUILD_ID-$(v)).ips $(DISTDIR)/exefs/$(v)/ ;)
	# the SD card: the translation template (tools/lang_template.py), sd:/sloperinaGZ/lang/
	mkdir -p $(DISTDIR)/sd/$(NAME)/lang && cp lang/template.json $(DISTDIR)/sd/$(NAME)/lang/

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(DISTDIR)

.PHONY: all dist clean FORCE
