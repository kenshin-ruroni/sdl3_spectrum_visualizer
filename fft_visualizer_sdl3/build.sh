#!/bin/bash

current_date_time=`date +"%Y-%m-%d %H:%M:%S"`;
echo $current_date_time " starting building application in " $(pwd)


if  test -d build; then
  rm -rf build
fi

if ! test -d build; then
  echo "creating build directory..."
  cmake -DCMAKE_BUILD_TYPE=Debug  -S . -B build
fi


cmake --build build  --verbose
current_date_time=`date +"%Y-%m-%d %H:%M:%S"`;
echo $current_date_time "building application ended " $(pwd)