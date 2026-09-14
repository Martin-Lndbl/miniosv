# modules/mininet: the network stack.
#
# There are two ways to link it, because there are two kinds of caller.
#
#   $(mininet-objects)      A Rust application that already depends on the
#                           crate -- the smoltcp-s3 benchmark does -- and so
#                           has the stack inside its own archive. All it needs
#                           from here is the shim, which is C++ and has to be
#                           compiled by the kernel.
#
#   $(mininet-lib-objects)  Everything else. Builds the crate as a staticlib
#                           and links it, so a C++ application talks to the
#                           stack through mininet.hh and never sees Rust.
#
# Include from an application's Makefile fragment and add one of them:
#
#     include modules/mininet/mininet.mk
#     app-objects += $(mininet-lib-objects)

mininet-dir := modules/mininet

# The shim is the only thing in the tree that includes the minidpdk headers
# directly; everything above it goes through the API.
$(out)/$(mininet-dir)/shim/shim.o: CXXFLAGS += -Iinclude/api/minidpdk

mininet-objects = $(mininet-dir)/shim/shim.o

# --- ring's C sources -------------------------------------------------------
#
# rustls' crypto provider is ring, and ring compiles a handful of C files
# through cc-rs. Those include <assert.h>, <string.h> and friends, which the
# devshell's clang cannot find on its own: this is a freestanding build and
# carries no host headers by design. Hand it the same headers the kernel's own
# C is compiled against, so the crate and the kernel agree on what a `size_t`
# is. Headers only -- cc-rs already passes -DNDEBUG, so no assert lands in the
# object and nothing here needs a host libc at link time.
#
# Exported because cargo passes the environment through to build scripts, and
# cc-rs reads the per-target variable before the generic one.
#
# $(out)/gen/include is in the list for the same reason it is in the kernel's:
# <bits/alltypes.h> is generated there, and include/api/stddef.h reaches for it.
ring-cflags = -isystem $(CURDIR)/include/api -isystem $(CURDIR)/include/api/$(arch) \
              -isystem $(CURDIR)/$(out)/gen/include
export CFLAGS_x86_64_unknown_linux_gnu = $(ring-cflags)
export CFLAGS_aarch64_unknown_linux_gnu = $(ring-cflags)

# --- the crate, for callers that are not Rust -------------------------------

mininet_cargo_dir = $(out)/mininet-objs/cargo
mininet_lib = $(mininet_cargo_dir)/release/libmininet.a

# Phony because cargo decides what needs rebuilding; make cannot know the
# crate's inputs without duplicating them here and getting it wrong.
.PHONY: $(mininet_lib)
$(mininet_lib):
	$(call quiet, cargo build --release --manifest-path $(mininet-dir)/rust/Cargo.toml \
		--target-dir $(mininet_cargo_dir), CARGO $(mininet-dir)/rust)

# The archive is handed to the linker under a .o name, as the app fragments do:
# the link line takes objects, and ld is happy to be given an archive there.
$(out)/mininet-objs/mininet.o: $(mininet_lib)
	$(makedir)
	$(call quiet, cmp -s $< $@ || cp $< $@, CP libmininet.a)

mininet-lib-objects = mininet-objs/mininet.o $(mininet-dir)/mininet.o $(mininet-objects)
