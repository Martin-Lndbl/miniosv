# The .o path the top-level Makefile expects
RUST_LIB = rust_app/target/$(mode)/libosv_app.a
OBJ = $(out)/rust_app/rust_app.o

$(OBJ): $(RUST_LIB)
	mkdir -p $(dir $(OBJ))
	cp $(RUST_LIB) $(OBJ)

$(RUST_LIB):
	cd rust_app && cargo build --$(mode)

app-objects = rust_app/rust_app.o

.PHONY: $(RUST_LIB)
