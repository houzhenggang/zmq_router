rm -f bm
gcc test.c ../build/libnetinet.a ../dpdk_libs/libdpdk.a build/lib/libperfm2 -lpthread -lrt -ldl -o bm
