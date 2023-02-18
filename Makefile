CXX=clang++
CXXFLAGS=-lpthread -Werror -gdwarf-4

all: run

minimake: test.cpp task.hpp
	$(CXX) $(CXXFLAGS) test.cpp -o test

run: test
	@./test
