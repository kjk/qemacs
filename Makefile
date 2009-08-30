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
CFLAGS += -DHAVE_QE_CONFIG_H -DCONFIG_ALL_KMAPS -DCONFIG_UNICODE_JOIN -DCONFIG_ALL_MODES

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

QE_OBJ = $(patsubst %.c $OUTDIR/QE_%.o, ${QE_SRC})
QE_DEP = $(patsubst %.o, %.d, $(QE_OBJ))
