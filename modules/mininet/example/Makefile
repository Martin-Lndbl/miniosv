# A C++ application on mininet, for exercising mininet.hh without DuckDB in
# the way.
#
#     make app=modules/mininet/example \
#         MININET_HOST=bucket.s3.eu-north-1.amazonaws.com \
#         MININET_ADDR=3.5.216.240
#
# There is no resolver in the guest, so the address is a build-time constant
# here exactly as it is for the benchmark. If it goes stale the run fails with
# a SYN timeout rather than silently.

example-dir := modules/mininet/example

include modules/mininet/mininet.mk

MININET_HOST ?= example.invalid
MININET_ADDR ?= 0.0.0.0
MININET_PATH ?= /blob.bin

$(out)/$(example-dir)/main.o: CXXFLAGS += \
	-DMININET_HOST=\"$(MININET_HOST)\" \
	-DMININET_ADDR=\"$(MININET_ADDR)\" \
	-DMININET_PATH=\"$(MININET_PATH)\"

# Those are target-specific variables, and make does not rebuild an object
# when one changes -- it only compares timestamps. A stale address does not
# fail loudly, it dials the wrong host, so record the values in a file and
# depend on that. Same reason the crate's build.rs declares its env vars.
mininet-example-stamp = $(out)/$(example-dir)/config.stamp
.PHONY: mininet-example-phony
$(mininet-example-stamp): mininet-example-phony
	$(call very-quiet, $(makedir))
	@v='$(MININET_HOST) $(MININET_ADDR) $(MININET_PATH)'; \
	 [ "$$(cat $@ 2>/dev/null)" = "$$v" ] || echo "$$v" > $@

$(out)/$(example-dir)/main.o: $(mininet-example-stamp)

app-objects = $(example-dir)/main.o
app-objects += $(mininet-lib-objects)
