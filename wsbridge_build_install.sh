#!/bin/sh

# absolute path to Freeswitch-1.6.20 source tree

#git clone -b v1.6.20 --single-branch https://freeswitch.org/stash/scm/fs/freeswitch.git
#git checkout -b v1.6.20-branch
if [$1 == ""]; then
	echo "This script needs the absolute path to Freeswitch Source Tree directory"
	echo "You might want to try this:"
	echo "mkdir /tmp/work_dir; cd /tmp/work_dir"
	echo "git clone -b v1.6.20 --single-branch https://freeswitch.org/stash/scm/fs/freeswitch.git"
	echo "git checkout -b v1.6.20-branch"
	echo "...and then run again ./wsbridge_build_install.sh from the wsbridge directory."
	exit
fi

cp -r mod_wsbridge/ $1/src/mod/endpoints/
cd $1 
echo Changed directory to $1
cp $OLDPWD/configure.ac.wsbridge.diff .
cat configure.ac.wsbridge.diff | patch -p1
apt-get install libwebsockets libwebsockets-dev
./bootstrap.sh
./configure
