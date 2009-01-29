#!/bin/bash

SCRIPT_BIN=`dirname $0`
. $SCRIPT_BIN/common.sh

let debug=0
let do_start=0
let do_stop=0
let select_mon=0
let select_mds=0
let select_osd=0
let localhost=0
let noselection=1
norestart=""
valgrind=""
MON_ADDR=""

usage="usage: $0 < start | stop | restart > [option]... [mon] [mds] [osd]\n"
usage=$usage"options:\n"
usage=$usage"\t-d, --debug\n"
usage=$usage"\t--norestart\n"
usage=$usage"\t--valgrind\n"
usage=$usage"\t-m ip:port\t\tspecify monitor address\n"
usage=$usage"\t--conf_file filename\n"

usage_exit() {
	printf "$usage"
	exit
}

while [ $# -ge 1 ]; do
case $1 in
	-d | --debug )
	debug=1
	;;
        -l | --localhost )
	localhost=1
	;;
	--norestart )
	norestart="--norestart"
	;;
	--valgrind )
	valgrind="--valgrind"
	;;
	mon | cmon )
	select_mon=1
	noselection=0
	;;
	mds | cmds )
	select_mds=1
	noselection=0
	;;
	osd | cosd )
	select_osd=1
	noselection=0
	;;
	start )
	[ $do_stop -eq 1 ] && usage_exit
	do_start=1
	;;
	stop )
	[ $do_start -eq 1 ] && usage_exit
	do_stop=1
	;;
	restart )
	[ $do_start -eq 1 ] && usage_exit
	[ $do_stop -eq 1 ] && usage_exit
	do_start=1
	do_stop=1
	;;
	-m )
	[ "$2" == "" ] && usage_exit
	MON_ADDR=$2
	shift
	;;
	--conf_file )
	[ "$2" == "" ] && usage_exit
	startup_conf_file=$2
	shift
	;;
	* )
	usage_exit
esac
shift
done

[ $do_start -eq 0 ] && [ $do_stop -eq 0 ] && usage_exit

if [ $do_stop -eq 1 ]; then
	stop_str=""
	[ $select_mon -eq 1 ] && stop_str=$stop_str" mon"
	[ $select_mds -eq 1 ] && stop_str=$stop_str" mds"
	[ $select_osd -eq 1 ] && stop_str=$stop_str" osd"
	$SCRIPT_BIN/stop.sh $stop_str
fi

[ $do_start -eq 0 ] && exit


[ "$startup_conf_file" == "" ] && startup_conf_file="startup.conf"

CCONF="$CCONF_BIN --conf_file $startup_conf_file"

if [ $noselection -eq 1 ]; then
	select_mon=1
	select_mds=1
	select_osd=1
fi

get_val CEPH_NUM_MON "$CEPH_NUM_MON" global num_mon 3
get_val CEPH_NUM_OSD "$CEPH_NUM_OSD" global "osd num" 1
get_val CEPH_NUM_MDS "$CEPH_NUM_MDS" global num_mds 3

ARGS="-f"

if [ $debug -eq 0 ]; then
	CMON_ARGS="--debug_mon 10 --debug_ms 1"
	COSD_ARGS=""
	CMDS_ARGS="--debug_ms 1"
else
	echo "** going verbose **"
	CMON_ARGS="--lockdep 1 --debug_mon 20 --debug_ms 1 --debug_paxos 20"
	COSD_ARGS="--lockdep 1 --debug_osd 25 --debug_journal 20 --debug_filestore 10 --debug_ms 1" # --debug_journal 20 --debug_osd 20 --debug_filestore 20 --debug_ebofs 20
	CMDS_ARGS="--lockdep 1 --mds_cache_size 500 --mds_log_max_segments 2 --debug_ms 1 --debug_mds 20 --mds_thrash_fragments 0 --mds_thrash_exports 1"
fi

get_val MON_ADDR "$MON_ADDR" global mon_addr 3

if [ "$MON_ADDR" != "" ]; then
	CMON_ARGS=$CMON_ARGS" -m "$MON_ADDR
	COSD_ARGS=$COSD_ARGS" -m "$MON_ADDR
	CMDS_ARGS=$CMDS_ARGS" -m "$MON_ADDR
fi


# lockdep everywhere?
# export CEPH_ARGS="--lockdep 1"

$SUDO rm -f core*

test -d out || mkdir out
$SUDO rm -f out/*
test -d gmon && $SUDO rm -rf gmon/*


# figure machine's ip
if [ $localhost -eq 1 ]; then
    IP="127.0.0.1"
else
    HOSTNAME=`hostname`
    echo hostname $HOSTNAME
    IP=`host $HOSTNAME | grep 'has address' | cut -d ' ' -f 4`
fi
echo "ip $IP"

[ "$CEPH_BIN" == "" ] && CEPH_BIN=.
[ "$CEPH_PORT" == "" ] && CEPH_PORT=6789

#mon
if [ $select_mon -eq 1 ]; then
	for f in `seq 0 $((CEPH_NUM_MON-1))`; do
		get_conf mon_data_path mondata "mon data path" mon$mon mon global
		get_conf mon_data_file mon$mon "mon data file" mon$mon mon global
		get_conf conf_file $startup_conf_file "conf file" mon$mon mon global

		get_conf ssh_host "" "ssh host" mon$mon mon global
		[ "$ssh_host" != "" ] && SSH_HOST="ssh $ssh_host" || SSH_HOST=""
		get_conf cd_path "" "ssh path" mon$mon mon global
		[ "$ssh_host" != "" ] && CD_PATH="cd $cd_path \\;" || CD_PATH=""

		$SSH_HOST $CD_PATH \
		$CEPH_BIN/crun $norestart $valgrind $CEPH_BIN/cmon $ARGS $CMON_ARGS $mon_data_path/$mon_data_file &
	done
	sleep 1
fi

#osd
if [ $select_osd -eq 1 ]; then
	for osd in `seq 0 $((CEPH_NUM_OSD-1))`
	do
		get_conf_bool use_sudo 0 sudo osd$osd osd global
		get_conf osd_dev dev/osd$osd "osd dev" osd$osd osd global
		get_conf conf_file $startup_conf_file "conf file" osd$osd osd global
		get_conf CEPH_PORT $CEPH_PORT "mon port" osd$osd osd global
		get_conf CEPH_HOST $IP "mon host" osd$osd osd global

		[ "$use_sudo" != "0" ] && SUDO="sudo" || SUDO=""

		get_conf ssh_host "" "ssh host" osd$osd osd global
		[ "$ssh_host" != "" ] && SSH_HOST="ssh $ssh_host" || SSH_HOST=""
		get_conf cd_path "" "ssh path" osd$osd osd global
		[ "$ssh_host" != "" ] && CD_PATH="cd $cd_path \\;" || CD_PATH=""

		echo start osd$osd
		$SSH_HOST $CD_PATH \
		$CEPH_BIN/crun $norestart $valgrind $SUDO $CEPH_BIN/cosd --conf_file $conf_file \
			-m $CEPH_HOST:$CEPH_PORT $osd_dev $ARGS $COSD_ARGS &
	done
fi

# mds
if [ $select_mds -eq 1 ]; then
	for mds in `seq 0 $((CEPH_NUM_MDS-1))`
	do
		get_conf conf_file $startup_conf_file "conf file" mds$mds mds global
		get_conf ssh_host "" "ssh host" mds$mds mds global
		[ "$ssh_host" != "" ] && SSH_HOST="ssh $ssh_host" || SSH_HOST=""
		get_conf cd_path "" "ssh path" mds$mds mds global
		[ "$ssh_host" != "" ] && CD_PATH="cd $cd_path \\;" || CD_PATH=""

		$SSH_HOST $CD_PATH \
		$CEPH_BIN/crun $norestart $valgrind $CEPH_BIN/cmds --conf_file $conf_file \
			-m $CEPH_HOST:$CEPH_PORT $ARGS $CMDS_ARGS &
	done
	$CEPH_BIN/ceph mds set_max_mds $CEPH_NUM_MDS
fi

echo "started. stop.sh to stop.  see out/* (e.g. 'tail -f out/????') for debug output."

