#!/bin/sh

SRTVER="1.2.3"
[ -d srt-$SRTVER ] || {
	wget https://github.com/Haivision/srt/archive/v$SRTVER.tar.gz || { 
		echo "can't download SRT."; 
		exit 1;
	}
	tar -xvf v$SRTVER.tar.gz
}

cd srt-$SRTVER && {
	./configure --prefix=../srt
	make && make install
} || {
	echo "missing SRT sources"
}
