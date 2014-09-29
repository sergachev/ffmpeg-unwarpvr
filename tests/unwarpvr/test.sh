#!/bin/bash
set -o errexit
if [ "`uname -s`" = 'Linux' ]
then
    pushd ../.. && make ffmpeg && popd
else
    pushd ../.. && make ffmpeg.exe && popd
fi
../../ffmpeg -y -i dk2.mp4 -ss 0 -vframes 1 -f image2 input.png
../../ffmpeg -y -i dk2.mp4 -v verbose -vf unwarpvr=2364:1461 -ss 0 -vframes 1 -f image2 output_full.png
../../ffmpeg -y -i dk2.mp4 -v verbose -vf unwarpvr=2500:1600 -ss 0 -vframes 1 -f image2 output_big.png
../../ffmpeg -y -i dk2.mp4 -v verbose -vf unwarpvr=1920:1080 -ss 0 -vframes 1 -f image2 output_cropped.png
../../ffmpeg -y -i dk2.mp4 -v verbose -vf unwarpvr=1920:1080 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_cropped.mp4
../../ffmpeg -y -i dk2.mp4 -v verbose -vf unwarpvr=2364:1462 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_full.mp4
# ../../ffmpeg -y -i dk2.mp4 -v verbose -vf unwarpvr=2500:1600 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_big.mp4
# ../../ffmpeg -y -i tuscany_eyerelief10_clip.mp4 -v verbose -vf unwarpvr=2364:1462 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p tuscany_eyerelief10_clip_dewarped.mp4
# ../../ffmpeg -y -i lava_clip_min.mp4 -v verbose -vf unwarpvr=1920:1080 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_cropped_min.mp4
# ../../ffmpeg -y -i lava_clip_min.mp4 -v verbose -vf unwarpvr=2364:1462 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_full_min.mp4
# ../../ffmpeg -y -i output_full.mp4 -v verbose -vf unwarpvr=1920:1080:forward_warp=1 -ss 0 -vframes 1 -f image2 input_full.png
# ../../ffmpeg -y -i lava_clip_min.mp4 -v verbose -vf "unwarpvr=2364:1462 , unwarpvr=1280:800:forward_warp=1:device=RiftDK1" -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_dk1_min.mp4
# 1576x974
