CROSS = arm-phoenix-

MAKE = make

CC = $(CROSS)gcc
LD = $(CROSS)ld
STRIP = $(CROSS)strip

ROOT_DIR = $(CURDIR)
MBEDTLS = $(ROOT_DIR)/gw/mbedtls

INCLUDES = -I$(MBEDTLS)/include
# LDLIBS += $(MBEDTLS)/library/libmbedx509.a $(MBEDTLS)/library/libmbedtls.a $(MBEDTLS)/library/libmbedcrypto.a
LDLIBS += $(PREFIX_O)libmbedx509.a $(PREFIX_O)libmbedtls.a $(PREFIX_O)libmbedcrypto.a
LDFLAGS += -z stack-size=12288

SOURCES = gateway.c https.c

OBJS = $(SOURCES:.c=.o)

all: $(addprefix $(PREFIX_PROG_STRIPPED), gateway)

mbedtls_make:
	$(MAKE) -C gw/mbedtls/library all

$(PREFIX_PROG)gateway: $(addprefix $(PREFIX_O), $(OBJS))
	@for dir in $(MBEDTLS); do \
		$(MAKE) -C $$dir clean; \
		if [ $$? != 0 ]; then exit 1; fi; \
	done
	$(MAKE) -C gw/mbedtls/library all
	cp gw/mbedtls/library/*.a $(PREFIX_O)
	@echo Linking: $@ ....
	@echo $(LINK)
	$(LINK)
	# $(STRIP) -s $@

$(PREFIX_O)gateway.o: gw/gateway.c
	@mkdir -p $(@D)
	$(SIL)(printf "CC  %-24s\n" "$<")
	$(SIL)$(CC) -c $(CFLAGS) -fstack-usage $(INCLUDES) "$<" -o "$@"

$(PREFIX_O)https.o: gw/https.c
	@mkdir -p $(@D)
	$(SIL)(printf "CC  %-24s\n" "$<")
	$(SIL)$(CC) -c $(CFLAGS) -fstack-usage $(INCLUDES) "$<" -o "$@"

mbedtls_clean:
	@for dir in $(MBEDTLS); do \
		$(MAKE) -C $$dir clean; \
		if [ $$? != 0 ]; then exit 1; fi; \
	done
