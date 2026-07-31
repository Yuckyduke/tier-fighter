# tier-fighter build.
#
# Plain Make rather than CMake -- the project is small enough that a configure step
# earns nothing. Targets:
#
#   make            build everything raylib allows (tests always, game/editor if found)
#   make test       build and run both test suites
#   make play       build and launch the game
#   make poses      build and launch the pose editor
#   make clean      remove build output
#
# Object files and binaries land in build/, which is gitignored.

CXX      ?= c++
CXXSTD    = -std=c++20
WARN      = -Wall -Wextra
OPT      ?= -O2

# --- Determinism flags: NON-NEGOTIABLE for the simulation --------------------
# -ffast-math lets the compiler reorder and approximate arithmetic, which breaks
# bit-identical determinism and therefore breaks rollback netcode. The sim uses no
# floats today, so this is mostly belt-and-braces -- but it must never be dropped.
# ffp-contract=off stops fused multiply-add from silently changing results too.
DETERMINISM = -fno-fast-math -ffp-contract=off

BUILD = build
INC   = -Isrc

# --- raylib detection --------------------------------------------------------
# Graphical targets are optional: if raylib is not installed, the tests still build.
# Mirrors the CMake behaviour of warning and skipping rather than failing.
RAYLIB_PREFIX := $(firstword $(wildcard /opt/homebrew /usr/local /usr))
HAVE_RAYLIB   := $(wildcard $(RAYLIB_PREFIX)/include/raylib.h)

RAYLIB_INC = -I$(RAYLIB_PREFIX)/include
RAYLIB_LIB = -L$(RAYLIB_PREFIX)/lib -lraylib

# Platform-specific frameworks raylib needs.
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
    RAYLIB_LIB += -framework Cocoa -framework IOKit -framework CoreVideo
else
    RAYLIB_LIB += -lm -lpthread -ldl
endif

# --- Sources -----------------------------------------------------------------
SIM_SRC    = src/sim/sim.cpp src/sim/arena.cpp src/sim/trig.cpp
RENDER_SRC = src/render/skeleton.cpp src/render/stickfigure.cpp

SIM_OBJ    = $(SIM_SRC:%.cpp=$(BUILD)/%.o)
RENDER_OBJ = $(RENDER_SRC:%.cpp=$(BUILD)/%.o)

# Test and graphical binaries.
TEST_BINS = $(BUILD)/tf_tests $(BUILD)/tf_arena_tests
GFX_BINS  = $(BUILD)/tf_play $(BUILD)/tf_poses

# --- Top-level targets -------------------------------------------------------
.PHONY: all test play poses clean

ifeq ($(HAVE_RAYLIB),)
all: $(TEST_BINS)
	@echo "raylib not found under $(RAYLIB_PREFIX) -- built tests only."
	@echo "Install it (brew install raylib / apt install libraylib-dev) for tf_play and tf_poses."
else
all: $(TEST_BINS) $(GFX_BINS)
endif

# Build then run both suites. The '&&' means a failing suite fails the target.
test: $(TEST_BINS)
	@echo "=== sim tests ===";   ./$(BUILD)/tf_tests
	@echo "=== arena tests ==="; ./$(BUILD)/tf_arena_tests

play: $(BUILD)/tf_play
	./$(BUILD)/tf_play

poses: $(BUILD)/tf_poses
	./$(BUILD)/tf_poses

# --- Object compilation ------------------------------------------------------
# The simulation objects get the determinism flags; nothing else needs them, but
# applying them everywhere is harmless and keeps the rule simple. Auto-generated
# .d files track header dependencies so a header edit rebuilds what uses it.
$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(OPT) $(DETERMINISM) $(INC) $(RAYLIB_INC) -MMD -MP -c $< -o $@

# --- Test binaries (sim only, no graphics) -----------------------------------
$(BUILD)/tf_tests: $(BUILD)/tests/test_sim.o $(SIM_OBJ)
	$(CXX) $(CXXSTD) $^ -o $@

$(BUILD)/tf_arena_tests: $(BUILD)/tests/test_arena.o $(SIM_OBJ)
	$(CXX) $(CXXSTD) $^ -o $@

# --- Graphical binaries (sim + render + raylib) ------------------------------
$(BUILD)/tf_play: $(BUILD)/src/render/main.o $(RENDER_OBJ) $(SIM_OBJ)
	$(CXX) $(CXXSTD) $^ $(RAYLIB_LIB) -o $@

$(BUILD)/tf_poses: $(BUILD)/src/render/pose_editor.o $(RENDER_OBJ) $(SIM_OBJ)
	$(CXX) $(CXXSTD) $^ $(RAYLIB_LIB) -o $@

clean:
	rm -rf $(BUILD)

# Pull in auto-generated header-dependency files.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
