CXX = clang++
CXXFLAGS = -std=c++14 -O2
FRAMEWORKS = -framework Cocoa -framework WebKit -framework ApplicationServices

all: build/carbon_pet build/power_tracker

build:
	mkdir -p build

build/carbon_pet: src/main.mm | build
	$(CXX) $(CXXFLAGS) $(FRAMEWORKS) src/main.mm -o build/carbon_pet

build/power_tracker: src/power_tracker.cpp | build
	$(CXX) $(CXXFLAGS) src/power_tracker.cpp -o build/power_tracker

run-pet: build/carbon_pet
	./build/carbon_pet

run-tracker: build/power_tracker
	./build/power_tracker

clean:
	rm -rf build/*
