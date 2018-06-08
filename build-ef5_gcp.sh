#!/bin/bash

# 1. create the EF5/build folder
# 2. Run this script from within the build folder
#	cd build
#	../build-ef5_gcp.sh


module load pgi/18.4 #openmpi/2.1.2/pgi/18.4
spack load libgeotiff

GEOTIFF=$(spack find --paths libgeotiff | tail -n 1 | awk '{print $2}')

cd .. && autoreconf --force --Install
cd build
#Compile for GPU
CC=pgcc CXX=pgc++ LDFLAGS="-L${GEOTIFF}/lib -lgeotiff" CPPFLAGS="-I${GEOTIFF}/include" CXXFLAGS+=" -O3 -std=c++11 -Minfo=accel -Mlarge_arrays -ta=tesla:managed" ../configure
#Compile for CPU Multicore
#CC=pgcc CXX=pgc++ LDFLAGS="-L${GEOTIFF}/lib -lgeotiff" CPPFLAGS="-I${GEOTIFF}/include" CXXFLAGS+=" -std=c++11 -Minfo=accel -ta=multicore" ../configure
make -j
