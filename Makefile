MESON ?= meson
BUILD_DIR ?= build

.PHONY: all
all: compile

.PHONY: build
build:
	@if [ ! -d "$(BUILD_DIR)" ]; then $(MESON) setup "$(BUILD_DIR)"; fi

.PHONY: compile
compile: build
	$(MESON) compile -C "$(BUILD_DIR)"

.PHONY: run
run: compile
	./$(BUILD_DIR)/bmenu

.PHONY: test
test: compile
	./test-bmenu.sh

.PHONY: clean
clean:
	@if [ -d "$(BUILD_DIR)" ]; then $(MESON) compile -C "$(BUILD_DIR)" --clean; fi

.PHONY: distclean
distclean:
	rm -rf "$(BUILD_DIR)"

.PHONY: rebuild
rebuild: distclean all
