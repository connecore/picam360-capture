OBJS=picam360_capture.o mrevent.o video.o mjpeg_decoder.o video_direct.o gl_program.o auto_calibration.o \
	omxcv_jpeg.o omxcv.o device.o view_coordinate_mpu9250.o picam360_tools.o \
	MotionSensor/libMotionSensor.a libs/libI2Cdev.a libs/freetypeGlesRpi/libFreetypeGlesRpi.a\
	plugins/driver_agent/driver_agent.o plugins/driver_agent/kokuyoseki.o plugins/driver_agent/rtp.o
BIN=picam360-capture.bin
LDFLAGS+=-lilclient -ljansson

include Makefile.include


