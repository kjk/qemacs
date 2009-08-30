# CFG=[rel|dbg]

VPATH+=src_c

ifeq ($(CFG),)
CFG=dbg
endif

INCS = -I src_c

OUTDIR=obj-$(CFG)

cc-option = $(shell if $(CC) $(OP_CFLAGS) $(1) -S -o /dev/null -xc /dev/null \
              > /dev/null 2>&1; then echo "$(1)"; else echo "$(2)"; fi ;)

CFLAGS += -g -Wall -funsigned-char
CFLAGS += $(call cc-option, -Wno-pointer-sign, "")

ifeq ($(CFG),dbg)
CFLAGS += -O0 -fno-inline ${INCS}
endif

ifeq ($(CFG),rel)
CFLAGS += -Os ${INCS} -DNDEBUG
endif

CFLAGS += -std=c99 -DHAVE_C99
CFLAGS += -DHAVE_QE_CONFIG_H -DCONFIG_ALL_KMAPS \
	-DCONFIG_UNICODE_JOIN -DCONFIG_ALL_MODES

CFLAGS += ${INCS}

LDFLAGS += -lm

QE_SRC = \
	qe.c charset.c buffer.c input.c unicode_join.c \
	display.c util.c hex.c list.c json.c strbuf.c \
	cutils.c unix.c tty.c charsetmore.c \
	charset_table.c unihex.c clang.c latex-mode.c \
	xml.c bufed.c shell.c dired.c arabic.c indic.c \
	qfribidi.c

ifdef CONFIG_X11
QE_SRC += x11.c
endif

QE_OBJ = $(patsubst %.c, ${OUTDIR}/QE_%.o, ${QE_SRC})
QE_DEP = $(patsubst %.o, %.d, $(QE_OBJ))
QE_APP = ${OUTDIR}/qe

all: inform ${OUTDIR} ${QE_APP}

$(OUTDIR):
	@mkdir -p $(OUTDIR)

$(QE_APP): ${QE_OBJ}
	$(CC) -g -o $@ $^ ${LDFLAGS}

$(OUTDIR)/QE_%.o: %.c
	$(CC) -MD -c $(CFLAGS) -o $@ $<

-include $(QE_DEP)

inform:
ifneq ($(CFG),rel)
ifneq ($(CFG),dbg)
	@echo "Invalid configuration: '"$(CFG)"'"
	@echo "Valid configurations: rel, dbg (e.g. make CFG=dbg)"
	@exit 1
endif
endif

clean: force
	rm -rf ${OUTDIR}

force:
