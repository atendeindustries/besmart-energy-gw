CROSS = arm-phoenix-

MAKE = make

CC = $(CROSS)gcc
LD = $(CROSS)ld

ROOT_DIR = $(CURDIR)
PREFIX_LIB = $(PREFIX_BUILD)/lib
MBEDTLS = $(ROOT_DIR)/gw/mbedtls

INCLUDES = -I$(MBEDTLS)/include
LDLIBS += $(PREFIX_O)libmbedx509.a $(PREFIX_O)libmbedtls.a $(PREFIX_O)libmbedcrypto.a
LDLIBS += $(PREFIX_LIB)/libiothub_client.a $(PREFIX_LIB)/libiothub_client_mqtt_transport.a $(PREFIX_LIB)/libumqtt.a $(PREFIX_LIB)/libaziotsharedutil.a
# libphoenix functions used in azure iot sdk aren't visible by default
LDLIBS += $(PREFIX_LIB)/libphoenix.a
LDFLAGS += -z stack-size=12288
CFLAGS += -Wformat-truncation=0

SOURCES = gateway.c https.c azure.c

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
	$(Q)$(LD) -o $@ $^ $(LIB) $(LDLIBS) $(LDFLAGS)

$(PREFIX_O)gateway.o: gw/gateway.c
	@mkdir -p $(@D)
	$(SIL)(printf "CC  %-24s\n" "$<")
	$(SIL)$(CC) -c $(CFLAGS) -fstack-usage $(INCLUDES) "$<" -o "$@"

$(PREFIX_O)https.o: gw/https.c
	@mkdir -p $(@D)
	$(SIL)(printf "CC  %-24s\n" "$<")
	$(SIL)$(CC) -c $(CFLAGS) -fstack-usage $(INCLUDES) "$<" -o "$@"

$(PREFIX_O)azure.o: gw/azure.c
	@mkdir -p $(@D)
	$(SIL)(printf "CC  %-24s\n" "$<")
	$(SIL)$(CC) -c $(CFLAGS) -fstack-usage $(INCLUDES) "$<" -o "$@"

mbedtls_clean:
	@for dir in $(MBEDTLS); do \
		$(MAKE) -C $$dir clean; \
		if [ $$? != 0 ]; then exit 1; fi; \
	done

include $(binary.mk)
