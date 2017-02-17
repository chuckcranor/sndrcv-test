#!/bin/bash -eu
#
#MSUB -N transport-test
#MSUB -l walltime=0:15:00
#MSUB -l nodes=1:haswell
#MSUB -o /users/$USER/joblogs/transport-test-$MOAB_JOBID.out
#MSUB -j oe
##MSUB -V
##MSUB -m b
##MSUB -m $USER@lanl.gov

# Notes on script operation
# 1) We are only using one node. That's it.
# 2) Any temporary output is directed to /tmp

######################
# Tunable parameters #
######################

umbrella_bin_dir="$HOME/src/deltafs-umbrella/install/bin"

###############
# Core script #
###############

logfile=$(mktemp)
server="$umbrella_bin_dir/sndrcv-srvr"
client="$umbrella_bin_dir/sndrcv-client"

message () { echo "$@" | tee -a $logfile; }
die () { message "Error $@"; exit 1; }

rm $logfile
message "Output is available in $logfile"

host1=$(/share/testbed/bin/emulab-listall | \
        awk -F, '{ print $1 "'"`hostname | sed 's/^[^\.]*././'`"'"}')
host2=$(/share/testbed/bin/emulab-listall | \
        awk -F, '{ print $2 "'"`hostname | sed 's/^[^\.]*././'`"'"}')

# Get host IPs
mpirun.openmpi -np 1 --host $host1 hostname -i > /tmp/host1-ip.txt
mpirun.openmpi -np 1 --host $host2 hostname -i > /tmp/host2-ip.txt
host1_ip=$(cat /tmp/host1-ip.txt | head -1)
host2_ip=$(cat /tmp/host2-ip.txt | head -1)
message "Host 1: hostname = $host1, ip = $host1_ip"
message "Host 2: hostname = $host2, ip = $host2_ip"

protos=("bmi+tcp" "cci+tcp" "cci+gni")
instances=(1 2 4 8)
repeats=3

run_one() {
    proto="$1"
    num="$2"
    iter=$3

    message ""
    message "====================================================="
    message "Testing protocol '$proto' with $num Mercury instances"
    message "Iteration $iter out of $repeats"
    message "====================================================="
    message ""

    address1="${proto}://$host1_ip:%d"
    address2="${proto}://$host2_ip:%d"

    # Start the server
    message "Starting server (Instances: $num, Address spec: $address1)."
    mpirun.openmpi -np 1 --host $host1 -tag-output $server $num $address1 \
        2>&1 >> $logfile &

    server_pid=$!

    # Start the client
    message "Starting client (Instances: $num, Address spec: $address2)."
    message "Please be patient while the test is in progress..."
    mpirun.openmpi -np 1 --host $host2 -tag-output $client $num $address2 \
        $address1 2>&1 >> $logfile

    # Collect return codes
    client_ret=$?
    wait $server_pid
    server_ret=$?

    if [[ $client_ret != 0 || $server_ret != 0 ]]; then
        if [ $client_ret != 0 ]; then
            message "Error: client returned $client_ret."
        fi
        if [ $server_ret != 0 ]; then
            message "Error: server returned $client_ret."
        fi
    else
        message "Test completed successfully."
    fi
}

for proto in ${protos[@]}; do
    for num in ${instances[@]}; do
        # BMI doesn't do well with >1 instances, so avoid those tests
        if [[ $proto == "bmi+tcp" && $num -gt 1 ]]; then
            continue;
        fi

        i=1
        while [ $i -le $repeats ]; do
            run_one $proto $num $i
            i=$((i + 1))
        done
    done
done
