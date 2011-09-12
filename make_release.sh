#!/bin/sh
#########################################
# This script is used by the maintainer
# to make a release of the software.
# Alan Ott
# Signal 11 Software
# under contract to PI Engineering
# 2011-09-12
#########################################


if [ $# -ne 1 ]; then
	echo "Usage: $0 VERSION_NUMBER"
	echo "For example: "
	echo "    $0 1.0.0"
	exit 1;
fi

export VERSION=$1
export TAG_NAME=xkeys-$VERSION

git tag $TAG_NAME
if [ $? -ne 0 ]; then
	echo "Unable to create tag. Exiting."
	echo "If you want to remove this tag, run"
	echo "    git tag -d $TAG_NAME"
	exit 1;
fi


git archive --format tar --prefix $TAG_NAME/ $TAG_NAME |gzip >../$TAG_NAME.tar.gz
if [ $? -ne 0 ]; then
	echo Unable to create release archive. Exiting.
	exit 1;
fi

echo ""
echo "Release is located in ../$TAG_NAME.tar.gz"
echo "Once you have tested it, run"
echo "    git push origin $TAG_NAME"
echo "If you want to discard this tag, run"
echo "    git tag -d $TAG_NAME"
