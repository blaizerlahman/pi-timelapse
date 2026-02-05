CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra $(shell pkg-config --cflags libcamera)
LDFLAGS = $(shell pkg-config --libs libcamera) -lpthread -ljpeg

all: timelapse

timelapse: main.o timelapse.o
	$(CXX) $(CXXFLAGS) -o timelapse main.o timelapse.o $(LDFLAGS)

main.o: main.cpp timelapse.h
	$(CXX) $(CXXFLAGS) -c main.cpp

timelapse.o: timelapse.cpp timelapse.h
	$(CXX) $(CXXFLAGS) -c timelapse.cpp

clean:
	rm -f *.o timelapse


