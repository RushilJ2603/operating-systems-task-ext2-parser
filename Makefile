# Build for the ext2 parser.
#
# Two source files and a couple of flag sets is the point where typing the
# g++ line by hand starts producing mistakes -- most importantly, accidentally
# comparing an -O2 build against an -O0 one.
#
#   make              build ./ext2fs
#   make debug        build with -O0 -g and the address/UB sanitizers
#   make check        run it over the disk image as a smoke test
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
LDFLAGS  ?= -pthread

BIN   := ext2fs
SRC   := src/ext2fs.cpp src/main.cpp
OBJ   := $(SRC:.cpp=.o)
DEPS  := src/ext2.h src/ext2fs.h
IMAGE ?= Artifacts/disk-backpup.img

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Sanitizers catch the out-of-bounds reads that binary parsing invites.
debug: CXXFLAGS := -std=c++17 -O0 -g -Wall -Wextra -fsanitize=address,undefined -pthread
debug: LDFLAGS  := -fsanitize=address,undefined -pthread
debug: clean $(BIN)

check: $(BIN)
	./$(BIN) $(IMAGE) super
	@echo
	./$(BIN) $(IMAGE) groups
	@echo
	./$(BIN) $(IMAGE) tree

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all debug check clean
