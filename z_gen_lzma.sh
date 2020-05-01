#!/bin/bash

set -e

help()
{
    echo -e "usage: $0 [raw file] [lzma out file]"
    exit 1;
}


if [ $# != 2 ]; then
    help
fi

./lzma_enc/lzma e $1 $2
