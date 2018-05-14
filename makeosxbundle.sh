INSTALLDIR="${1:-RTMPtoSRT-osx}"

#just to remember building instructions
[ -f MonaSRT ] || {
	export CFLAGS="-I"$(brew --prefix openssl)"/include"
	make
}

rm -fr $INSTALLDIR
mkdir -p $INSTALLDIR

cp -a MonaSRT RTMPtoSRT
export DYLD_FALLBACK_FRAMEWORK_PATH=./
dylibbundler -od -b -x ./RTMPtoSRT -d $INSTALLDIR/libs/ -p @executable_path/libs
mv RTMPtoSRT $INSTALLDIR

#not sure why this is needed but it is. probably a bug in dylibbundler.
[ -f $INSTALLDIR/libs/libssl.1.0.0.dylib ] && {
	install_name_tool -change /usr/local/Cellar/openssl/1.0.2o_1/lib/libcrypto.1.0.0.dylib @executable_path/libs/libcrypto.1.0.0.dylib $INSTALLDIR/libs/libssl.1.0.0.dylib
}
