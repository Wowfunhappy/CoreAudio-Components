# EAC3 Audio Component Makefile

CC = gcc
CFLAGS = -arch x86_64 -fPIC -O2 -Wall \
         -F/System/Library/Frameworks \
         -I./EAC3AudioComponent/ffmpeg/include \
         -DNDEBUG

LDFLAGS = -arch x86_64 -bundle -flat_namespace -undefined suppress \
          -framework CoreFoundation \
          -framework CoreAudio \
          -framework AudioUnit \
          -framework AudioToolbox \
          -L./EAC3AudioComponent/ffmpeg/lib \
          -lavcodec -lavutil -lswresample

TARGET = build/EAC3.component

SOURCES = EAC3AudioComponent/Source/EAC3AudioComponent.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	@echo "Building EAC3 Audio Component..."
	@mkdir -p build/EAC3.component/Contents/MacOS
	@mkdir -p build/EAC3.component/Contents/Resources
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/EAC3.component/Contents/MacOS/EAC3AudioComponent $(SOURCES)
	@cp EAC3AudioComponent/Resources/Info.plist build/EAC3.component/Contents/
	@echo "Build complete!"

install: $(TARGET)
	@echo "Installing EAC3 Audio Component..."
	@mkdir -p ~/Library/Audio/Plug-Ins/Components
	@rm -rf ~/Library/Audio/Plug-Ins/Components/EAC3.component
	@cp -R build/EAC3.component ~/Library/Audio/Plug-Ins/Components/
	@echo "Installation complete!"

clean:
	@echo "Cleaning..."
	@rm -rf build
	@echo "Clean complete!"

test: install
	@echo "Testing with example file..."
	@if [ -f ../Reference/*.mp4 ] || [ -f ../Reference/*.mov ] || [ -f ../Reference/*.m4v ]; then \
		open -a "QuickTime Player" ../Reference/*.mp4 ../Reference/*.mov ../Reference/*.m4v 2>/dev/null || true; \
	else \
		echo "No test file found in Reference folder"; \
	fi

.PHONY: all install clean test