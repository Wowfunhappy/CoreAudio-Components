# DTS Audio Component Makefile

CC = clang
CFLAGS = -arch x86_64 -mmacosx-version-min=10.6 -fPIC -O2 -Wall \
         -F/System/Library/Frameworks \
         -I./DTSAudioComponent/ffmpeg/include \
         -DNDEBUG

LDFLAGS = -arch x86_64 -mmacosx-version-min=10.6 -bundle \
          -framework CoreFoundation \
          -framework CoreAudio \
          -framework AudioUnit \
          -framework AudioToolbox \
          -L./DTSAudioComponent/ffmpeg/lib \
          -lavcodec -lavutil -lswresample

# Bundle path contains a space, which GNU make handles poorly as a target/prereq, so the
# bundle is assembled entirely inside the recipe (where the shell quotes it correctly).
BUNDLE = build/WFH DTS.component
SOURCES = DTSAudioComponent/Source/DTSAudioComponent.c
INSTALL_DIR = $(HOME)/Library/Audio/Plug-Ins/Components

all:
	@echo "Building DTS Audio Component..."
	@mkdir -p "$(BUNDLE)/Contents/MacOS"
	@mkdir -p "$(BUNDLE)/Contents/Resources"
	$(CC) $(CFLAGS) -o "$(BUNDLE)/Contents/MacOS/DTSAudioComponent" $(SOURCES) $(LDFLAGS)
	@cp DTSAudioComponent/Resources/Info.plist "$(BUNDLE)/Contents/"
	@echo "Build complete!"

install: all
	@echo "Installing DTS Audio Component..."
	@mkdir -p "$(INSTALL_DIR)"
	@rm -rf "$(INSTALL_DIR)/WFH DTS.component"
	@cp -R "$(BUNDLE)" "$(INSTALL_DIR)/"
	@echo "Installation complete!"

clean:
	@echo "Cleaning..."
	@rm -rf build
	@echo "Clean complete!"

# Rebuilds the bundled FFmpeg static libraries (libavcodec/libavutil/libswresample)
# with only the DTS (DCA) decoder enabled. Only needed if the ffmpeg/ folder is missing.
ffmpeg:
	./build-ffmpeg.sh

.PHONY: all install clean ffmpeg
