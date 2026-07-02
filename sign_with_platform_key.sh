#! /bin/bash

set -e

help()
{
    echo "$0 arg1 arg2"
    echo "arg1 (keydir) arg2 (apk to sign)"
}

echo
if [ $# -lt 2 ]
then
    echo "missing arguments"
    help
    exit 1
fi

KEY_DIR=$1
APK_DIR=`dirname $2`
APK_NAME=`basename $2`
PK8=$1/platform.pk8
X509=$1/platform.x509.pem

if [ ! -f $PK8 ] || [ ! -f $X509 ]
then
    echo "key $PK8 or $X509 does not exist"
    help
    exit 1
fi

if [ ! -f $2 ]
then
    echo "apk $2 does not exist"
    help
    exit 3
fi

SIGNED_APK_NAME="signed-"$APK_NAME

if [ -f $APK_DIR/$SIGNED_APK_NAME ]
then
    echo "$APK_DIR/$SIGNED_APK_NAME already exist, remove it first"
    help
    exit 3
fi

CUR_DIR=$PWD

# /home/builduser/opt/SDK/build-tools/34.0.0/apksigner 
apksigner sign --key $PK8 --cert $X509 --in $APK_DIR/$APK_NAME --out $APK_DIR/$SIGNED_APK_NAME
file $APK_DIR/$SIGNED_APK_NAME
