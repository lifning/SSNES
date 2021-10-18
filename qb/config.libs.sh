. qb/qb.libs.sh

check_switch_c C99 -std=gnu99
check_critical C99 "Cannot find C99 compatible compiler."

# There are still broken 64-bit Linux distros out there. :)
if [ -d /usr/lib64 ]; then
   add_library_dirs /usr/lib64
fi

if [ -d /opt/local/lib ]; then
   add_library_dirs /opt/local/lib
fi

if [ $OS = BSD ]; then
   DYLIB=-lc
else
   DYLIB=-ldl
fi

if [ -z "$LIBSNES" ]; then
   LIBSNES="-lsnes"
else
   echo "Explicit libsnes used, disabling dynamic libsnes loading ..."
   HAVE_DYNAMIC=no
fi

if [ $HAVE_DYNAMIC != yes ]; then
   check_lib_cxx SNES $LIBSNES snes_init $DYLIB
   check_critical SNES "Cannot find libsnes."
   add_define_make libsnes $LIBSNES
fi

check_lib THREADS -lpthread pthread_create
check_lib DYLIB $DYLIB dlopen

check_lib NETPLAY -lc socket
if [ $HAVE_NETPLAY = yes ]; then
   HAVE_GETADDRINFO=auto
   check_lib GETADDRINFO -lc getaddrinfo
   if [ $HAVE_GETADDRINFO = yes ]; then
      HAVE_SOCKET_LEGACY=no
   else
      HAVE_SOCKET_LEGACY=yes
   fi
fi

check_lib GETOPT_LONG -lc getopt_long

if [ $HAVE_DYLIB = no ] && [ $HAVE_DYNAMIC = yes ]; then
   echo "Dynamic loading of libsnes is enabled, but your platform does not appear to have dlopen(), use --disable-dynamic or --with-libsnes=\"-lsnes\"".
   exit 1
fi

check_pkgconf ALSA alsa
check_header OSS sys/soundcard.h
check_header OSS_BSD soundcard.h
check_lib OSS_LIB -lossaudio

if [ $OS = Darwin ]; then
   check_lib AL "-framework OpenAL" alcOpenDevice
else
   check_lib AL -lopenal alcOpenDevice
fi

if [ $OS = Darwin ]; then
   check_lib FBO "-framework OpenGL" glFramebufferTexture2D
else
   check_lib FBO -lGL glFramebufferTexture2D
fi

check_pkgconf RSOUND rsound 1.1
check_pkgconf ROAR libroar
check_pkgconf JACK jack 0.120.1
check_pkgconf PULSE libpulse

check_lib COREAUDIO "-framework AudioUnit" AudioUnitInitialize

check_pkgconf SDL sdl 1.2.10
check_critical SDL "Cannot find SDL library."

# On some distros, -lCg doesn't link against -lstdc++ it seems ...
if [ $HAVE_OPENGL != no ]; then
   check_lib_cxx CG -lCg cgCreateContext
else
   echo "Ignoring Cg. OpenGL is not enabled."
   HAVE_CG=no
fi

check_pkgconf XML libxml-2.0
check_pkgconf SDL_IMAGE SDL_image

if [ $HAVE_THREADS != no ]; then
   if [ $HAVE_FFMPEG != no ]; then
      check_pkgconf AVCODEC libavcodec
      check_pkgconf AVFORMAT libavformat
      check_pkgconf AVUTIL libavutil
      check_pkgconf SWSCALE libswscale

      ( [ $HAVE_FFMPEG = auto ] && ( [ $HAVE_AVCODEC = no ] || [ $HAVE_AVFORMAT = no ] || [ $HAVE_AVUTIL = no ] || [ $HAVE_SWSCALE = no ] ) && HAVE_FFMPEG=no ) || HAVE_FFMPEG=yes
   fi

   if [ $HAVE_FFMPEG = yes ]; then
      check_lib FFMPEG_ALLOC_CONTEXT3 "$AVCODEC_LIBS" avcodec_alloc_context3
      check_lib FFMPEG_AVCODEC_OPEN2 "$AVCODEC_LIBS" avcodec_open2
      check_lib FFMPEG_AVCODEC_ENCODE_AUDIO2 "$AVCODEC_LIBS" avcodec_encode_audio2
      check_lib FFMPEG_AVIO_OPEN "$AVFORMAT_LIBS" avio_open
      check_lib FFMPEG_AVFORMAT_WRITE_HEADER "$AVFORMAT_LIBS" avformat_write_header
      check_lib FFMPEG_AVFORMAT_NEW_STREAM "$AVFORMAT_LIBS" avformat_new_stream
      check_lib FFMPEG_AVCODEC_ENCODE_VIDEO2 "$AVCODEC_LIBS" avcodec_encode_video2
   fi

   if [ $HAVE_FFMPEG = no ] && [ $HAVE_X264RGB = yes ]; then
      echo "x264 RGB recording is enabled, but FFmpeg is not. --enable-x264rgb will not have any effect."
   fi
else
   echo "Not building with threading support. Will skip FFmpeg."
   HAVE_FFMPEG=no
fi

check_lib DYNAMIC $DYLIB dlopen

check_pkgconf FREETYPE freetype2
check_pkgconf X11 x11
check_pkgconf XEXT xext
if [ $HAVE_X11 = yes ] && [ $HAVE_XEXT = yes ]; then
   check_pkgconf XVIDEO xv
else
   echo "X11 or Xext not present. Skipping XVideo."
   HAVE_XVIDEO=no
fi

check_lib STRL -lc strlcpy

check_pkgconf PYTHON python3
check_pkgconf PYTHON python3-embed

add_define_make OS $OS

# Creates config.mk and config.h.
VARS="ALSA OSS OSS_BSD OSS_LIB AL RSOUND ROAR JACK COREAUDIO PULSE SDL OPENGL DYLIB GETOPT_LONG THREADS CG XML SDL_IMAGE DYNAMIC FFMPEG AVCODEC AVFORMAT AVUTIL SWSCALE CONFIGFILE FREETYPE XVIDEO X11 XEXT NETPLAY SOCKET_LEGACY FBO STRL PYTHON FFMPEG_ALLOC_CONTEXT3 FFMPEG_AVCODEC_OPEN2 FFMPEG_AVIO_OPEN FFMPEG_AVFORMAT_WRITE_HEADER FFMPEG_AVFORMAT_NEW_STREAM FFMPEG_AVCODEC_ENCODE_AUDIO2 FFMPEG_AVCODEC_ENCODE_VIDEO2 X264RGB SINC BSV_MOVIE"
create_config_make config.mk $VARS
create_config_header config.h $VARS

