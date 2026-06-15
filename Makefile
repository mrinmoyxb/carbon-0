CXX = clang++
CXXFLAGS = -std=c++17 -O2
FRAMEWORKS = -framework Cocoa -framework WebKit -framework ApplicationServices
TOKEN_PROXY_FRAMEWORKS = -framework Security -framework CoreFoundation

all: build/carbon_pet build/power_tracker build/token_proxy

build:
	mkdir -p build

build/carbon_pet: src/main.mm | build
	$(CXX) $(CXXFLAGS) $(FRAMEWORKS) src/main.mm -o build/carbon_pet

build/power_tracker: src/power_tracker.cpp | build
	$(CXX) $(CXXFLAGS) src/power_tracker.cpp -o build/power_tracker

build/token_proxy: src/token_proxy.cpp | build
	$(CXX) $(CXXFLAGS) $(TOKEN_PROXY_FRAMEWORKS) src/token_proxy.cpp -o build/token_proxy

run-pet: build/carbon_pet
	./build/carbon_pet

run-tracker: build/power_tracker
	./build/power_tracker

run-proxy: build/token_proxy
	./build/token_proxy

clean:
	rm -rf build/*
