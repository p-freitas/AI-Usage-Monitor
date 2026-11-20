CXX      ?= g++
WINDRES  ?= windres
CXXFLAGS ?= -std=c++17 -Os -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type \
            -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
LDFLAGS  ?= -municode -mwindows -static -s -Wl,--gc-sections
LIBS     := -lcomctl32 -lole32 -loleaut32 -lgdi32 -lcrypt32
SOURCES  := $(wildcard src/*.cpp)
OBJECTS  := $(patsubst src/%.cpp,build/%.o,$(SOURCES)) build/resources.o
HEADERS  := $(wildcard src/*.h res/*.h)

AIUsageMonitor.exe: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

build/%.o: src/%.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/resources.o: res/app.rc res/resource.h res/app.manifest $(wildcard res/*.ico)
	@mkdir -p build
	$(WINDRES) -i $< -o $@ --include-dir res

test:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tests/ai_usage_test.cpp src/claude_config.cpp -o build/ai_usage_test.exe -static -lcrypt32
	./build/ai_usage_test.exe
	$(CXX) $(CXXFLAGS) tests/integration_test.cpp -o build/integration_test.exe -static -lcrypt32
	./build/integration_test.exe

.PHONY: test
