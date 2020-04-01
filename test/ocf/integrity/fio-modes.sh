#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

function fio_verify(){
	fio_bdev $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev "$@"
}

function cleanup(){
	rm -f $curdir/modes.conf
}

function clear_nvme()
{
        # Clear metadata on NVMe device
        $rootdir/scripts/setup.sh reset
        sleep 5
        name=$(get_nvme_name_from_bdf $1)

        mountpoints=$(lsblk /dev/$name --output MOUNTPOINT -n | wc -w)
        if [ "$mountpoints" != "0" ]; then
                $rootdir/scripts/setup.sh
                exit 1
        fi
        dd if=/dev/zero of=/dev/$name bs=1M count=1000 oflag=direct
        $rootdir/scripts/setup.sh
}

# Clear only nvme device which we will use in test
bdf=$($rootdir/scripts/gen_nvme.sh --json | jq '.config[0].params.traddr' | sed s/\"//g)

clear_nvme "$bdf"

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

nvme_cfg=$($rootdir/scripts/gen_nvme.sh)

config="
$nvme_cfg

[Split]
  Split Nvme0n1 8 101

[OCF]
  OCF PT_Nvme  pt Nvme0n1p0 Nvme0n1p1
  OCF WT_Nvme  wt Nvme0n1p2 Nvme0n1p3
  OCF WB_Nvme0 wb Nvme0n1p4 Nvme0n1p5
  OCF WB_Nvme1 wb Nvme0n1p6 Nvme0n1p7
"
echo "$config" > $curdir/modes.conf

fio_verify --filename=PT_Nvme:WT_Nvme:WB_Nvme0:WB_Nvme1 --spdk_conf=$curdir/modes.conf

trap - SIGINT SIGTERM EXIT
cleanup
