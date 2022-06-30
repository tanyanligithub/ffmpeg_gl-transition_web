 # configure FFMpeg with Emscripten
CFLAGS="-L../x264-snapshot-20170226-2245-stable/build/lib -lx264"
LDFLAGS="$CFLAGS  -L../x264-snapshot-20170226-2245-stable/build/lib -s INITIAL_MEMORY=2146435072"
CONFIG_ARGS=(
  --target-os=none        # use none to prevent any os specific configurations
  --arch=x86_32           # use x86_32 to achieve minimal architectural optimization
  --enable-cross-compile  # enable cross compile
  --disable-x86asm        # disable x86 asm
  --disable-inline-asm    # disable inline asm
  --disable-stripping     # disable stripping
  --disable-programs      # disable programs build (incl. ffplay, ffprobe & ffmpeg)
  --disable-ffmpeg 
  --disable-libfreetype
  --disable-devices
  --disable-avdevice 
  --disable-ffplay 
  --disable-ffprobe 
  --disable-doc 
  --disable-htmlpages 
  --disable-manpages 
  --disable-podpages 
  --disable-txtpages 
  --disable-postproc
  --disable-network
  --disable-doc           # disable doc
  --extra-cflags="-I./x264/include  -I./zlib/include -msimd128 -O3 --closure 1"
  --extra-cxxflags="-I./x264/include  -I./zlib/include  -msimd128 -O3 --closure 1"
  --extra-ldflags="-L./x264/lib  -L./zlib "
  --nm="llvm-nm"
  --ar=emar
  --ranlib=emranlib
  --cc=emcc
  --cxx=em++
  --objcc=emcc
  --dep-cc=emcc
   --enable-gpl 
   --disable-everything
   --enable-libx264
   --enable-decoder=mpeg4,h264,mjpeg,aac,ac3,ac3_fixed,,mp3,png #opus,vorbis
  --enable-encoder=libx264,mpeg4,ljpeg,mjpeg,aac,ac3,ac3_fixed,mp3,png #opus,vorbis,
  --enable-parser=h264
  --enable-protocol=file
  --enable-demuxer=mov,mp4,image2,avi,mp3
  --enable-muxer=mp4,flv,image2,avi,mp3
   --enable-filter=aresample 
    --enable-decoder=aac 
    --enable-decoder=aac_latm 
    --enable-decoder=aac_fixed 
    --enable-decoder=ac3 
    --enable-decoder=ac3_fixed 
    --enable-decoder=als 
    --enable-decoder=alac 
    --enable-decoder=mp1 
    --enable-decoder=mp1float 
    --enable-decoder=mp2 
    --enable-decoder=mp2float 
    --enable-decoder=mp3float 
    --enable-decoder=mp3adufloat 
    --enable-decoder=mp3adu 
    --enable-decoder=mp3on4 
    --enable-decoder=mp3on4float 
    --enable-decoder=als 
    # --enable-decoder=vorbis 
    # --enable-decoder=flac 
    # --enable-decoder=wavpack 
    # --enable-decoder=wmalossless 
    # --enable-decoder=wmapro 
    # --enable-decoder=wmav1 
    # --enable-decoder=wmav2 
    --enable-bsf=mp3_header_decompress 
    --enable-bsf=aac_adtstoasc 
    --enable-bsf=chomp 
    --enable-bsf=remove_extradata 
    --enable-swresample 
    --enable-parser=aac 
    --enable-parser=aac_latm 
    --enable-parser=ac3 
    --enable-parser=flac 
    --enable-parser=ac3 
    # --enable-parser=vorbis 
    --enable-parser=mpegaudio
    --enable-demuxer=aac 
    --enable-demuxer=ac3 
    --enable-demuxer=eac3 
    --enable-demuxer=aiff 
    --enable-demuxer=flac 
    --enable-demuxer=latm 
    --enable-demuxer=mp3 
    # --enable-demuxer=ogg 
    # --enable-demuxer=eac3 
    # --enable-demuxer=xwma 
    # --enable-demuxer=wav 
    # --enable-muxer=wav 
    --enable-encoder=aac 
  # --enable-sdl
  --enable-filters
  # --enable-filter=split
  # --enable-filter=trim
  # --enable-filter=concat
  # --enable-filter=setpts
  # --enable-filter=crop
  # --enable-filter=scale
  # --enable-filter=eq
  #  --enable-filter=lut3d
  # --enable-filter=gltransition 
  --extra-libs='-lGLEW -lglfw'
  --enable-parser=png,jpg,bmp
  --disable-pthreads  
  --enable-mmi
  --enable-zlib
  --disable-debug
  # --enable-bzlib
)
emconfigure ./configure "${CONFIG_ARGS[@]}"

# build dependencies
emmake make -j4
