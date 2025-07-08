# DTS Audio Component Makefile

CC = gcc
CFLAGS = -arch x86_64 -fPIC -O2 -Wall \
         -F/System/Library/Frameworks \
         -I./DTSAudioComponent/ffmpeg/include \
         -DNDEBUG

LDFLAGS = -arch x86_64 -bundle -flat_namespace -undefined suppress \
          -framework CoreFoundation \
          -framework CoreAudio \
          -framework AudioUnit \
          -framework AudioToolbox \
          -L./DTSAudioComponent/ffmpeg/lib \
          -lavcodec -lavutil -lswresample

TARGET = build/DTS.component

SOURCES = DTSAudioComponent/Source/DTSAudioComponent.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	@echo "Building DTS Audio Component..."
	@mkdir -p build/DTS.component/Contents/MacOS
	@mkdir -p build/DTS.component/Contents/Resources
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/DTS.component/Contents/MacOS/DTSAudioComponent $(SOURCES)
	@cp DTSAudioComponent/Resources/Info.plist build/DTS.component/Contents/
	@echo "Build complete!"

install: $(TARGET)
	@echo "Installing DTS Audio Component..."
	@mkdir -p ~/Library/Audio/Plug-Ins/Components
	@rm -rf ~/Library/Audio/Plug-Ins/Components/DTS.component
	@cp -R build/DTS.component ~/Library/Audio/Plug-Ins/Components/
	@echo "Installation complete!"

clean:
	@echo "Cleaning..."
	@rm -rf build
	@echo "Clean complete!"

test: install
	@echo "Testing with example file..."
	@if [ -f ../Reference/*.dts ] || [ -f ../Reference/*.mp4 ] || [ -f ../Reference/*.mov ] || [ -f ../Reference/*.m4v ]; then \
		open -a "QuickTime Player" ../Reference/*.dts ../Reference/*.mp4 ../Reference/*.mov ../Reference/*.m4v 2>/dev/null || true; \
	else \
		echo "No test file found in Reference folder"; \
	fi

.PHONY: all install clean test