include ../config.mak

CFLAGS:=-Wall -g $(CFLAGS)
ifdef TARGET_GPROF
CFLAGS+= -p
LDFLAGS+= -p
endif
ifdef TARGET_ARCH_X86
#CFLAGS+=-fomit-frame-pointer
ifeq ($(GCC_MAJOR),2)
CFLAGS+=-m386 -malign-functions=0
else
CFLAGS+=-march=i386 -falign-functions=0
endif
endif
CFLAGS+=-I..

OBJS=css.o xmlparse.o cssparse.o html_style.o docbook_style.o
LIB=libqhtml.a

all: $(LIB)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rc $@ $(OBJS)

#clean:
#	rm -f *~ *.o *.a *.exe csstoqe html_style.c docbook_style.c

clean:
	rm -f obj-$(CFG) *~ *.o *.a *.exe csstoqe docbook_style.c

cleanall:
	rm -rf obj-*

# build default style sheet file
csstoqe$(EXE): csstoqe.c
	$(HOST_CC) $(CFLAGS) -o $@ $<

html_style.c: html.css csstoqe$(EXE)
	./csstoqe html_style < $< > $@

docbook_style.c: docbook.css csstoqe$(EXE)
	./csstoqe docbook_style < $< > $@
