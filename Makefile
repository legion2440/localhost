RUST_BIN := target/release/localhost
CPP_BIN := bonus_cpp/localhost_cpp

all: rust

rust:
	cargo build --release

cpp:
	$(MAKE) -C bonus_cpp

test:
	cargo test

check-config: rust
	./$(RUST_BIN) --check-config -c localhost.conf

audit: rust
	python3 tests/audit.py $(RUST_BIN)

audit-cpp: cpp
	python3 tests/audit.py $(CPP_BIN)

stress:
	python3 tests/stress.py 8080 1000 50

clean:
	cargo clean
	$(MAKE) -C bonus_cpp clean

.PHONY: all rust cpp test check-config audit audit-cpp stress clean
