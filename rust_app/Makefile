# OSv fails during boot if we build the debug target
RUST_LIB = rust_app/target/release/libosv_app.a
OBJ = $(out)/rust_app/rust_app.o
$(OBJ): $(RUST_LIB)
	mkdir -p $(dir $(OBJ))
	cp $(RUST_LIB) $(OBJ)
$(RUST_LIB):
	cd rust_app && cargo build --release

# C++ shim: bridges the C++-only minidpdk API (rte_eth_dev virtual
# methods, eth_os::get_eth_for_port()) to the extern "C" surface the
# Rust port calls into. See rust_app/shim/shim.h for the rationale.
SHIM_SRC = rust_app/shim/shim.cc
SHIM_OBJ = $(out)/rust_app/shim/shim.o

$(SHIM_OBJ): $(SHIM_SRC)
	mkdir -p $(dir $(SHIM_OBJ))
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Iinclude/api/minidpdk -c $< -o $@

app-objects = rust_app/rust_app.o rust_app/shim/shim.o
.PHONY: $(RUST_LIB)
