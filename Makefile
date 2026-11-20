CXX      ?= g++
WINDRES  ?= windres
CXXFLAGS ?= -std=c++17 -Os -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type \
            -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
LDFLAGS  ?= -municode -mwindows -static -s -Wl,--gc-sections
LIBS     := -lcomctl32 -lole32 -loleaut32 -lgdi32 -lcrypt32 -lwinhttp -lbcrypt -lversion
OFFICIAL ?= 0
OBJDIR   := build/$(if $(filter 1,$(OFFICIAL)),release,dev)
TESTCXX  = $(CXX) $(CXXFLAGS)
SOURCES  := $(wildcard src/*.cpp)
OBJECTS  := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SOURCES)) $(OBJDIR)/resources.o
HEADERS  := $(wildcard src/*.h res/*.h)

AIUsageMonitor.exe: $(OBJDIR)/AIUsageMonitor.exe FORCE
	cp $< $@

$(OBJDIR)/AIUsageMonitor.exe: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

$(OBJDIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -DAUM_OFFICIAL_RELEASE=$(OFFICIAL) -c $< -o $@

$(OBJDIR)/resources.o: res/app.rc $(HEADERS) res/app.manifest $(wildcard res/*.ico)
	@mkdir -p $(OBJDIR)
	$(WINDRES) -i $< -o $@ --include-dir res

test: $(OBJDIR)/resources.o
	@mkdir -p build
	$(TESTCXX) tests/ai_usage_test.cpp src/claude_config.cpp -o build/ai_usage_test.exe -static -lcrypt32
	./build/ai_usage_test.exe
	$(TESTCXX) tests/integration_test.cpp src/ai_usage.cpp -o build/integration_test.exe -static -lcrypt32
	./build/integration_test.exe
	$(TESTCXX) tests/update_core_test.cpp src/update_core.cpp -o build/update_core_test.exe -static
	./build/update_core_test.exe
	$(TESTCXX) tests/update_signature_test.cpp src/update_signature.cpp -o build/update_signature_test.exe -static -lbcrypt
	./build/update_signature_test.exe
	$(TESTCXX) tests/update_win_test.cpp src/update_core.cpp src/update_signature.cpp $< -o build/update_win_test.exe -static $(LIBS)
	./build/update_win_test.exe

.PHONY: test FORCE
FORCE:
