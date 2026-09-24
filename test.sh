#!/bin/sh -e

export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
corm=./bin/corm

assert() {
	file=snap/$1.txt
	shift
	echo $@ >&2
	if "$@" | diff $file -; then
		return 0;
	else
		echo Test FAILED! $file != $@ >&2
		return 1
	fi
}

rm *.db 2>/dev/null || true

./bin/test | diff expects.txt -
./bin/test_extended
./bin/test_multivalue
./bin/test_record
./bin/rec_test
./bin/rec_axis_test
./bin/rec_axis_store_test
./bin/rec_cli_test

adb=a.db:a:u
bdb=b.db:a
sdb=s.db
cdb=c.db:2u:u
ddb=d.db
assert 0 $corm -p 0:1 $adb
assert 01v $corm -p hi -p hello $bdb
assert 01 $corm -l $adb
assert 1hello0hi $corm -l $bdb

assert none $corm -g hallo $bdb
assert 0 $corm -g hi $bdb
assert none $corm -g 2 $bdb
assert none $corm -g 0 $bdb

assert justhi $corm -rg hallo $bdb
assert justhi $corm -rg hi $bdb
assert none $corm -rg 2 $bdb
assert justhi $corm -rg 0 $bdb

assert 0 $corm -q $bdb -g hi $bdb # 0 key
assert 0 $corm -q $bdb -g hallo $bdb # 0 key
assert none $corm -q $bdb -g 2 $bdb
assert 0 $corm -q $bdb -g 0 $bdb

assert justhi $corm -q $bdb -rg hi $bdb
assert none $corm -q $bdb -rg hallo $bdb
assert none $corm -q $bdb -rg 2 $bdb
assert none $corm -q $bdb -rg 0 $bdb

assert 0 $corm -q $bdb -q $bdb -g hi $bdb
assert none $corm -q $bdb -q $bdb -g hallo $bdb
assert none $corm -q $bdb -q $bdb -g 2 $bdb
assert none $corm -q $bdb -q $bdb -g 0 $bdb

assert justhi $corm -q $bdb -q $bdb -rg hi $bdb
assert justhi $corm -q $bdb -q $bdb -rg hallo $bdb
assert none $corm -q $bdb -q $bdb -rg 2 $bdb
assert justhi $corm -q $bdb -q $bdb -rg 0 $bdb

assert hikey $corm -p 'hi:Hi how are you' $sdb
assert hi $corm -l $sdb
assert hi2key $corm -p 'hi2:Hi how are you 2' $sdb
assert empty $corm -rd 'hi2' $sdb
assert hi $corm -l $sdb
assert hi2key $corm -p 'hi2:Hi how are you 2' $sdb
assert empty $corm -d 'Hi how are you 2' $sdb
assert hi $corm -l $sdb

# FIXME No DUP PRIMARY support yet
assert 50 $corm -p 5:9 -p 0:1 $cdb # but this still works
# assert missing $corm -q $adb -L $cdb
# assert empty $corm -q $cdb -L $adb
# assert 01 $corm -q $cdb -rL $adb
assert 5 $corm -p 5:9 $adb
assert empty $corm -d 9 $adb
assert 01 $corm -l $adb
assert 5 $corm -p 5:9 $adb
assert empty $corm -d 5:9 $adb
assert 01 $corm -l $adb
assert 5 $corm -p 5:9 $adb
assert empty $corm -rd 9:5 $adb
assert 01 $corm -l $adb
assert 55 $corm -p 5:9 -p 5:8 $adb
assert empty $corm -rd 5 $adb
assert 01 $corm -l $adb

assert assoc $corm -a $sdb -rl $bdb
assert assoc-bail $corm -a $bdb -l $adb
assert assoc3 $corm -a $bdb -a $sdb -l $adb
assert assoc-bail $corm -xa $bdb -a $sdb -l $adb
assert assoc-bail2 $corm -xa $bdb -a $sdb -rl $adb

assert hikey $corm -p hi:Hello2 $ddb
assert hi2key $corm -p hi2:Hello3 $ddb
assert xxd-1 xxd $ddb

# TODO test random (normal and reverse?)
# TODO test AINDEX col insert id
