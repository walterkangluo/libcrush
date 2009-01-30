#!/bin/bash

let new=0
let debug=0
let stopfirst=1
let ramjournal=0
norestart="--norestart"

while [ $# -ge 1 ]; do
    case $1 in
        -d | --debug )
            debug=1
	    ;;
        --new | -n )
            new=1
	    ;;
        --restart | -n )
            norestart=""
	    ;;
        --norestart | -n )
            norestart="--norestart"
	    ;;
	--nostop )
	    stopfirst=0
	    ;;
	--ramjournal )
	    ramjournal=1
	    ;;
    esac
    shift
done


ARGS="--dout_dir /data/`hostname`"

MOUNTOPTIONS="-o notreelog,flushoncommit"

if [ $debug -eq 0 ]; then
    CMON_ARGS="--debug_mon 10 --debug_ms 1"
    COSD_ARGS=""
    CMDS_ARGS="--file_layout_pg_size 3 --debug_ms 1"
else
    echo "** going verbose **"
    CMON_ARGS="--lockdep 1 --debug_mon 20 --debug_ms 1 --debug_paxos 20"
    COSD_ARGS="--lockdep 1 --debug_osd 20 --debug_journal 20 --debug_filestore 0 --debug_ms 1" # --debug_journal 20 --debug_osd 20 --debug_filestore 20 --debug_ebofs 20
    CMDS_ARGS="--file_layout_pg_size 3 --lockdep 1 --mds_cache_size 500 --mds_log_max_segments 2 --debug_ms 1 --debug_mds 20 --mds_thrash_fragments 0 --mds_thrash_exports 0"
fi


if [ $stopfirst -eq 1 ]; then
    ./dstop.sh
fi

if [ $new -eq 1 ]; then
    # build and inject an initial osd map
    ./osdmaptool --clobber --createsimple 32 --num_dom 4 .ceph_osdmap

    # use custom crush map to separate data from metadata
    ./crushtool -c cm.txt -o cm
    ./osdmaptool --clobber --import-crush cm .ceph_osdmap

#    ./ceph osd setmap 2 -i .ceph_osdmap
fi

# mkmonfs
if [ $new -eq 1 ]; then

    # clean up
    echo removing old core files
    rm -f core*

    echo removing old logs
    rm -f log/*

    echo removing old output
    test -d out || mkdir out
    rm -f out/* /data/cosd*/*

    test -d gmon && ssh cosd0 rm -rf ceph/src/gmon/*


    # figure machine's ip
    HOSTNAME=`hostname`
    IP=`host $HOSTNAME | grep $HOSTNAME | cut -d ' ' -f 4`

    echo hostname $HOSTNAME
    echo "ip $IP"
    if [ `echo $IP | grep '^127\\.'` ]
    then
	echo
	echo "WARNING: hostname resolves to loopback; remote hosts will not be able to"
	echo "  connect.  either adjust /etc/hosts, or edit this script to use your"
	echo "  machine's real IP."
	echo
    fi
    
    # build a fresh fs monmap, mon fs
    ./monmaptool --create --clobber --add $IP:6789 --print .ceph_monmap
    ./mkmonfs --clobber mondata/mon0 --mon 0 --monmap .ceph_monmap --osdmap .ceph_osdmap
fi

# monitor
./cmon -d mondata/mon0 $ARGS $CMON_ARGS

# osds
savelog -l cosd
cp -p cosd.0 cosd

for host in `cd dev/hosts ; ls`
do
 ssh root@cosd$host killall cosd

 test -d devm && ssh root@cosd$host modprobe btrfs  #crc32c \; insmod $HOME/src/btrfs-unstable/fs/btrfs/btrfs.ko

 for osd in `cd dev/hosts/$host ; ls`
 do
   dev="dev/hosts/$host/$osd"
   echo "---- host $host osd $osd dev $dev ----"
   devm="$dev"

   # btrfs?
   if [ -d devm ]; then
       devm="devm/osd$osd"
       echo "---- dev mount $devm ----"
       test -d $devm || mkdir -p $devm
       if [ $new -eq 1 ]; then
	   echo mkfs btrfs
	   ssh root@cosd$host cd $HOME/ceph/src \; umount $devm \; \
	       $HOME/src/btrfs-progs-unstable/mkfs.btrfs $dev \; \
	       mount -t btrfs $MOUNTOPTIONS $dev $devm
	   if [ $ramjournal -eq 1 ]; then
	       ssh root@cosd$host dd if=/dev/zero of=/r/osd$osd.journal bs=1048576 count=1 seek=128
           fi
       else
	   echo mounting btrfs
	   ssh root@cosd$host cd $HOME/ceph/src \; mount -t btrfs $MOUNTOPTIONS $dev $devm
       fi
   fi

   if [ $new -eq 1 ]; then
       echo mkfs
       ssh root@cosd$host cd $HOME/ceph/src \; ./cosd --mkfs_for_osd $osd $devm # --osd_auto_weight 1
   fi
   echo starting cosd
   ssh root@cosd$host cd $HOME/ceph/src \; ulimit -c unlimited \; ./crun $norestart ./cosd $devm --dout_dir /data/cosd$host $COSD_ARGS -f &

 done
done

# mds
./cmds $ARGS -d $CMDS_ARGS


