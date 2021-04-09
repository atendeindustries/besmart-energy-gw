CROSS = i386-pc-phoenix-

MAKE = make

CC = $(CROSS)gcc
LD = $(CROSS)ld
STRIP = $(CROSS)strip

ROOT_DIR = $(CURDIR)
MBEDTLS = $(ROOT_DIR)/mbedtls

LDFLAGS =

INCLUDES = -I$(MBEDTLS)/include
LIBS = $(MBEDTLS)/library/libmbedx509.a $(MBEDTLS)/library/libmbedtls.a $(MBEDTLS)/library/libmbedcrypto.a

SOURCES = gateway.c https.c

OBJS = $(SOURCES:.c=.o)

.SUFFIXES:.c .o

all: gateway

mbedtls_make:
	@for dir in $(MBEDTLS); do \
		$(MAKE) -C $$dir ; \
		if [ $$? != 0 ]; then exit 1; fi; \
	done

gateway: mbedtls_make $(OBJS) 
	@echo Linking: $@ ....
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)
	$(STRIP) -s $@

.c.o:
	@echo Compiling: $< ....
	$(CC) -c $(CFLAGS) $(INCLUDES) -o $@ $<

mbedtls_clean:
	@for dir in $(MBEDTLS); do \
		$(MAKE) -C $$dir clean; \
		if [ $$? != 0 ]; then exit 1; fi; \
	done
