#!/bin/bash

if [ -z ${CXX} ]
then
	export CXX=g++
fi

#scorep --verbose=2 g++ --std=c++14 test.cpp -o test
scorep --thread=none ${CXX} test.cpp -o test -pthread

