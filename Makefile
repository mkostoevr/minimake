CXX=clang++
CXXFLAGS=-lpthread -Werror -gdwarf-4

all: run

minimake: minimake.cpp minimake.hpp
	$(CXX) $(CXXFLAGS) minimake.cpp -o minimake

run: minimake
	@./minimake
