PREFIX ?= /usr/local
BUILD ?= build

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j2

install:
	cmake --install $(BUILD) --prefix $(PREFIX)

clean:
	rm -rf $(BUILD)
