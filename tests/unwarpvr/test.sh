#!/bin/bash
set -o errexit
pushd ../.. && make ffmpeg && popd
../../ffmpeg -y -i lava_clip_short.mp4 -ss 0 -vframes 1 -f image2 input.png
../../ffmpeg -y -i lava_clip_short.mp4 -vf unwarpvr=2364:1461 -ss 0 -vframes 1 -f image2 output_full.png
../../ffmpeg -y -i lava_clip_short.mp4 -vf unwarpvr=2500:1600 -ss 0 -vframes 1 -f image2 output_big.png
../../ffmpeg -y -i lava_clip_short.mp4 -vf unwarpvr=1920:1080 -ss 0 -vframes 1 -f image2 output_cropped.png
../../ffmpeg -y -i lava_clip_short.mp4 -vf unwarpvr=1920:1080 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_cropped.mp4
../../ffmpeg -y -i lava_clip_short.mp4 -vf unwarpvr=2364:1462 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p output_full.mp4
