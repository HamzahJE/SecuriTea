# Host tests for the pure .ir parser (ir_file.cpp). No board needed.
# The shim/ folder provides just enough of Arduino (String, File, vTaskDelay) to compile it on a PC.
#   make        build and run with AddressSanitizer and UndefinedBehaviorSanitizer
#   make clean
CXX ?= c++
ROOT := ../..
CXXFLAGS := -std=c++17 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
            -I shim -I $(ROOT)
BUILD := build

.PHONY: all run clean
all: run

$(BUILD)/ir_file_test: ir_file_test.cpp $(ROOT)/ir_file.cpp $(ROOT)/ir_file.h $(ROOT)/app_state.h shim/Arduino.h shim/FS.h
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) ir_file_test.cpp $(ROOT)/ir_file.cpp -o $@

run: $(BUILD)/ir_file_test
	./$(BUILD)/ir_file_test

clean:
	rm -rf $(BUILD)
