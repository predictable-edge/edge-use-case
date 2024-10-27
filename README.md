# edge-use-case
Requirements for the origin downloaded video 
1. slice each frame to 16. 
2. use bit depth 8. 
3. without B frames. 

ffmpeg -i lab-experiment.mp4 \
  -c:v libx264 \
  -preset slow \
  -crf 18 \
  -pix_fmt yuv420p \
  -bf 0 \
  -slices 16 \
  -c:a copy \
  lab-experiment-b8-slice16.mp4