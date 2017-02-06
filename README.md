# sndrcv-test
multi-instance mercury send/recv test

# server

The sndrcv-srvr.cc program contains a mercury RPC server that
receives and responds to "count" number of RPC requests and 
then exits.

It can run multiple instances of the mercury server
in the same process.   listening port numbers are assigned
sequentially starting at BASEPORT (defined in the code as 19900).
(the address spec uses a printf "%d" to fill the port number...)

```
   usage: ./sndrcv-srvr n-instances local-addr-spec
  
   example:
     ./sndrcv-srvr 1 bmi+tcp://10.93.1.154:%d      # 1 instance w/bmi
     ./sndrcv-srvr 1 cci+tcp://10.93.1.154:%d      # 1 instance w/cci
                                                   # both on port 19900
  
     ./sndrcv-srvr 2 cci+tcp://10.93.1.154:%d      # 2 instance w/cci
                                                   # ports=19900, 19901
```


# client

The sndrcv-client.cc program contains a mercury RPC client that sends
"count" number of RPC requests and exits when all the replies in.

It can run multiple instances of the mercury client
in the same process.   server port numbers are assigned
sequentially starting at BASEPORT (defined in the code as 19900).
(the address spec uses a printf "%d" to fill the port number...)
we init the client side with ports after that...

there are two sending modes: normally the client sends the
RPC requests in parallel, but if you set environment variable
 "SERIALSEND" it will wait an RPC request to complete before 
sending the next one.

note: the number of instances between the client and server
should match.

```
   usage: ./sndrcv-client n-instances local-addr-spec remote-addr-spec
  
   example:
     # server is on 10.93.1.154, local IP is 10.93.1.146
     ./sndrcv-client 1 bmi+tcp://10.93.1.146:%d bmi+tcp://10.93.1.154:%d
     ./sndrcv-client 1 cci+tcp://10.93.1.146:%d cci+tcp://10.93.1.154:%d
     # 1 instance, remote server port=19900, local port=19901
  
     ./sndrcv-client 1 cci+tcp://10.93.1.146:%d cci+tcp://10.93.1.154:%d
     # 2 instances, remote server port=19900,19901 local port=19902,19903
```

# compile

First, you need to know where mercury is installed and you need cmake.
To compile with a build subdirectory, starting from the top-level source dir:
```
  mkdir build
  cd build
  cmake -DCMAKE_PREFIX_PATH=/path/to/mercury-install ..
  make
```

That will produce binaries in the current directory.  "make install"
will install the binaries in CMAKE_INSTALL_PREFIX/bin (defaults to
/usr/local/bin) ... add a -DCMAKE_INSTALL_PREFIX=dir to change the
install prefix from /usr/local to something else.
