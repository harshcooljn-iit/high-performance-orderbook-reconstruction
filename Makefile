CXX       := clang++
CXXFLAGS  := -std=c++17 -O3 -march=native -flto -ffast-math \
             -funroll-loops -fomit-frame-pointer \
             -DNDEBUG -Wall -Wextra -Wpedantic

LDFLAGS   := -flto -lpthread

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/reconstruction

SRCS := $(SRC_DIR)/main.cpp
HDRS := $(wildcard $(SRC_DIR)/*.h)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	@echo ""
	@echo "═══════════════════════════════════════════════════════════"
	@echo "  Running Orderbook Reconstruction..."
	@echo "═══════════════════════════════════════════════════════════"
	@echo ""
	./$(TARGET) data/mbo.csv --reference data/mbp.csv

stress: $(TARGET)
	@echo ""
	@echo "═══════════════════════════════════════════════════════════"
	@echo "  Running Stress Test on 2M rows..."
	@echo "═══════════════════════════════════════════════════════════"
	@echo ""
	./$(TARGET) data/mbo_2M.csv

clean:
	rm -rf $(BUILD_DIR)
