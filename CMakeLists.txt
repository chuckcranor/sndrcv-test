cmake_minimum_required(VERSION 3.1)

project (sndrcv-test CXX)

set (CMAKE_THREAD_PREFER_PTHREAD TRUE)
set (THREADS_PREFER_PTHREAD_FLAG TRUE)
# XXX: cmake 3.1 and newer define a Threads::Threads imported target
# that we should switch too when we are ready to require 3.1 or better.
# (3.1 was released late 2014)
find_package (Threads REQUIRED)

find_package (mercury CONFIG REQUIRED)

add_executable (sndrcv-srvr sndrcv-srvr.cc)
target_include_directories (sndrcv-srvr PUBLIC ${MERCURY_INCLUDE_DIR})
target_link_libraries (sndrcv-srvr mercury Threads::Threads)

add_executable (sndrcv-client sndrcv-client.cc)
target_include_directories (sndrcv-client PUBLIC ${MERCURY_INCLUDE_DIR})
target_link_libraries (sndrcv-client mercury Threads::Threads)
