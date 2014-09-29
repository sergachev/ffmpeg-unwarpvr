#!/bin/bash
echo Basic DK2
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1920:1080 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Explicit eye relief dial
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1920:1080:eye_relief_dial=3 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Test single frame
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1920:1080 -ss 0 -vframes 1 -f image2 out.png
echo Test first few seconds
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1920:1080 -t 10 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Swap eyes
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1920:1080:swap_eyes=1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'YouTube 3D (height stretched by 2)'
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1920:1080:scale_height=2 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Full render target
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=2364:1462 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Mono view
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1280:720:left_eye_only=1:scale_width=1.2:scale_height=1.2 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Forward warping
ffmpeg-unwarpvr -i unwarped.mp4 -vf unwarpvr=1920:1080:forward_warp=1:ppd=15 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo Stretched 3D input
ffmpeg-unwarpvr -i unwarped.mp4 -vf unwarpvr=1920:1080:forward_warp=1:ppd=15:scale_in_height=2 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo Swapped eyes input
ffmpeg-unwarpvr -i unwarped.mp4 -vf unwarpvr=1920:1080:forward_warp=1:ppd=15:swap_eyes=1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo Mono input
ffmpeg-unwarpvr -i unwarped.mp4 -vf unwarpvr=1920:1080:forward_warp=1:ppd=15:mono_input=1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo Forward warp to DK1
ffmpeg-unwarpvr -i unwarped.mp4 -vf unwarpvr=1920:1080:forward_warp=1:ppd=15:device=RiftDK1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk1.mp4
echo Unwarp DK1
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1280:720:device=RiftDK1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Unwarp DK1 to 1080p
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1920:1080:device=RiftDK1:scale_width=1.5:scale_height=1.5 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Unwarp DK1 to 1440p
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=2560:1440:device=RiftDK1:scale_width=2:scale_height=2 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Unwarp cropped DK1
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1920:1080:device=RiftDK1:scale_width=1.5:scale_height=1.5:scale_in_height=1.111 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'YouTube 3D (height stretched by 2)'
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1920:1080:device=RiftDK1:scale_width=1.5:scale_height=3.0 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Full render target
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1900:1026:device=RiftDK1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'Full render target (1920x1200)'
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=2850:1538:device=RiftDK1:scale_width=1.5:scale_height=1.5 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'Mono view (360p)'
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=640:360:device=RiftDK1:left_eye_only=1 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'Mono view (480p)'
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=854:480:device=RiftDK1:left_eye_only=1:scale_width=1.25:scale_height=1.25 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'Mono view (720p)'
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1280:720:device=RiftDK1:left_eye_only=1:scale_width=1.85:scale_height=1.85 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'Mono view (1080p)'
ffmpeg-unwarpvr -i dk1.mp4 -vf unwarpvr=1920:1080:device=RiftDK1:left_eye_only=1:scale_width=2.8:scale_height=2.8 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo 'DK1->DK2'
ffmpeg-unwarpvr -i dk1.mp4 -vf "unwarpvr=1900:1026:device=RiftDK1 , unwarpvr=1920:1080:forward_warp=1:device=RiftDK2:ppd=6.84" -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo 'DK2->DK1'
ffmpeg-unwarpvr -i dk2.mp4 -vf "unwarpvr=2364:1462:device=RiftDK2 , unwarpvr=1280:800:forward_warp=1:device=RiftDK1:ppd=10.34" -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk1.mp4
echo 'DK1->DK2 high quality'
ffmpeg-unwarpvr -i dk1.mp4 -vf "unwarpvr=3800:2052:device=RiftDK1:scale_width=2:scale_height=2 , unwarpvr=1920:1080:forward_warp=1:device=RiftDK2:ppd=13.68" -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo 'DK1->DK2 highest quality'
ffmpeg-unwarpvr -i dk1.mp4 -vf "unwarpvr=3800:2052:device=RiftDK1:scale_width=2:scale_height=2 , unwarpvr=3840:2160:forward_warp=1:device=RiftDK2:ppd=13.68 , scale=1920:1080" -sws_flags lanczos+accurate_rnd+full_chroma_int -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk2.mp4
echo 'DK2->DK1 highest quality'
ffmpeg-unwarpvr -i dk2.mp4 -vf "unwarpvr=4728:2924:device=RiftDK2:scale_width=2:scale_height=2 , unwarpvr=2560:1600:forward_warp=1:device=RiftDK1:ppd=20.68 , scale=1280:800" -sws_flags lanczos+accurate_rnd+full_chroma_int -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out_dk1.mp4
echo Downscaling DK2 unwarping to 720p
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=1280:720:scale_width=0.6667:scale_height=0.6667 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Downscaling to 480p
ffmpeg-unwarpvr -i dk2.mp4 -vf unwarpvr=854:480:scale_width=0.4448:scale_height=0.4444 -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
echo Supersampling
ffmpeg-unwarpvr -i dk2.mp4 -sws_flags lanczos+accurate_rnd+full_chroma_int -vf "unwarpvr=3840:2160:scale_width=2.0:scale_height=2.0 , scale=1920:1080" -c:a copy -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4
