echo $1
echo $2
echo "processing"
rm -rf web/videos/$2
mkdir web/videos/$2
ffmpeg -re -i $1 -c:a aac -c:v libx264 \
-map 0 -b:v:1 6M -s:v:1 1920x1080 -profile:v:1 high \
-map 0 -b:v:0 144k -s:v:0 256x144 -profile:v:0 baseline \
-bf 1 -keyint_min 120 -g 120 -sc_threshold 0 -b_strategy 0 \
-ar:a:1 22050 -use_timeline 1 -use_template 1 \
-adaptation_sets "id=0,streams=v id=1,streams=a" -f dash web/videos/$2/dash.mpd