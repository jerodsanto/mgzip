#!/bin/sh

MGZIP=./mgzip
if [ -e /dev/zero ]; then 
	DEVZERO=/dev/zero
else
	echo "You don't have /dev/zero.  Making a big bunch of zeros to test with..." 
	awk 'BEGIN{for(i=0;i<800000;i++)printf "          ";exit}' | tr ' ' '\000' > bigzeros
	DEVZERO=`pwd`/bigzeros
	echo "  finished making zeros."
fi

die() 
{ 
	echo error in testing $@. 
	exit 1;
}

dd if=$DEVZERO of=testfile bs=1024 count=2048

# test the basics
time $MGZIP testfile
echo "(my PIII/400 single CPU reports 0.30user 0.03system 0:00.36elapsed)"

if [ -e testfile ]; then
die $MGZIP testfile
fi
gzip -d testfile.gz
if [ ! -e testfile ]; then 
die gzip -d testfile.gz
fi

# make sure multiple files work correctly
cp testfile testfile2
$MGZIP testfile testfile2 
if [ -e testfile ]; then
die $MGZIP testfile testfile2
fi
if [ -e testfile2 ]; then
die $MGZIP testfile testfile2
fi
if [ ! -e testfile.gz ]; then
die $MGZIP testfile testfile2
fi
if [ ! -e testfile2.gz ]; then 
die $MGZIP testfile testfile2
fi

# make sure -c works as expected
echo $MGZIP testfile testfile2
ls -la testfile*
gzip -d testfile*.gz
$MGZIP -c testfile testfile2 > testfile3.gz
if [ -e testfile.gz ]; then 
die $MGZIP -c testfile testfile2  
fi
if [ ! -s testfile3.gz ]; then 
die $MGZIP -c testfile testfile2  
fi
if [ ! -s testfile ]; then
die $MGZIP -c testfile testfile2  
fi
cat testfile testfile2 > testfile3
gzip -dc testfile3.gz > testfile
rm testfile2
if diff testfile testfile3; then 
echo consistency check 1 ok.
else
die $MGZIP / gzip consistency.
fi

# make sure $MGZIP with no parameters works
$MGZIP < testfile > testfile.gz
gzip -dc testfile.gz > testfile2
if diff testfile testfile2; then 
echo consistency check 2 ok.
else
die $MGZIP / gzip consistency.
fi

# make sure -p flag works 
rm testfile.gz
$MGZIP -p testfile 
if [ ! -e testfile.gz ]; then
die $MGZIP -p testfile
fi
if [ ! -e testfile ]; then
die $MGZIP -p testfile 
fi
echo $MGZIP passes all current tests.
rm testfile*

# check for a broken gzip
echo Checking to see if your gzip is broken...
dd if=$DEVZERO bs=256 count=29323 | gzip -1 > testfile.gz
TESTSIZE=`cat testfile.gz testfile.gz | gunzip | wc -c | xargs`
rm testfile*
if [ "$TESTSIZE" != "15013376" ]; then 
	echo "expected 15013376 bytes uncompressed; got $TESTSIZE instead."
	die " -- your gzip is broken.  Read the README file and then patch your gzip"
else
	echo Your gzip is fine.  You rock. 
fi

if [ -f $DEVZERO ]; then 
	rm $DEVZERO
fi
 
