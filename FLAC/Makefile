CC = clang
CFLAGS = -arch x86_64 -mmacosx-version-min=10.6 -O2 -fPIC -IFLACAudioComponent/include
LDFLAGS = -framework Carbon -framework AudioToolbox -framework CoreFoundation -framework CoreAudio -framework AudioUnit -LFLACAudioComponent/lib -lFLAC -dynamiclib -exported_symbols_list exports.txt

COMPONENT_NAME = FLACAudioComponent
BUNDLE_NAME = FLAC.component
SOURCE_DIR = FLACAudioComponent/Source
RESOURCE_DIR = FLACAudioComponent/Resources
BUILD_DIR = build

SOURCES = $(SOURCE_DIR)/FLACAudioComponent.c
OBJECTS = $(BUILD_DIR)/FLACAudioComponent.o
DYLIB = $(BUILD_DIR)/$(COMPONENT_NAME)

INSTALL_DIR = ~/Library/Audio/Plug-Ins/Components

all: $(BUILD_DIR) $(BUNDLE_NAME)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(DYLIB): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUNDLE_NAME): $(DYLIB)
	mkdir -p $(BUNDLE_NAME)/Contents/MacOS
	mkdir -p $(BUNDLE_NAME)/Contents/Resources
	cp $(DYLIB) $(BUNDLE_NAME)/Contents/MacOS/
	cp $(RESOURCE_DIR)/Info.plist $(BUNDLE_NAME)/Contents/
	touch $(BUNDLE_NAME)

install: $(BUNDLE_NAME)
	mkdir -p $(INSTALL_DIR)
	rm -rf $(INSTALL_DIR)/$(BUNDLE_NAME)
	cp -R $(BUNDLE_NAME) $(INSTALL_DIR)/

clean:
	rm -rf $(BUILD_DIR) $(BUNDLE_NAME)

uninstall:
	rm -rf $(INSTALL_DIR)/$(BUNDLE_NAME)

.PHONY: all install clean uninstall