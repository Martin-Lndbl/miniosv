# modules/mininet: the C++ half of the network stack.
#
# The Rust half is a cargo crate under rust/. Each application builds that
# itself -- cargo wants one target directory per final artifact, and the crate
# is a dependency rather than a linkable object on its own. This fragment
# supplies only what the kernel has to compile: the shim the crate calls into.
#
# Include from an application's Makefile fragment and add $(mininet-objects)
# to app-objects:
#
#     include modules/mininet/mininet.mk
#     app-objects += $(mininet-objects)

mininet-dir := modules/mininet

# The shim is the only thing in the tree that includes the minidpdk headers
# directly; everything above it goes through the Rust API.
$(out)/$(mininet-dir)/shim/shim.o: CXXFLAGS += -Iinclude/api/minidpdk

mininet-objects = $(mininet-dir)/shim/shim.o
