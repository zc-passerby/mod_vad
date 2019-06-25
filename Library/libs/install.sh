#!/bin/bash

echo Start Install required libs
tar zxvf webrtcLib.tar.gz -C /
cp yunfan_webrtc_media.h /usr/include
cp libWebrtcMediaHandle.pc /usr/lib64/pkgconfig
echo Completed Install required libs