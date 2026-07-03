# OSv fails during boot if we build the debug target

RUST_LIB = rust_app/target/release/libosv_app.a
OBJ = $(out)/rust_app/rust_app.o

$(OBJ): $(RUST_LIB)
	mkdir -p $(dir $(OBJ))
	cp $(RUST_LIB) $(OBJ)

$(RUST_LIB):
	cd rust_app && cargo build --release

app-objects = rust_app/rust_app.o

.PHONY: $(RUST_LIB)
