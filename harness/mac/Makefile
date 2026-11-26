# Builds Bada.app (universal arm64 + x86_64) with the Command Line Tools; no Xcode needed.
#   make            build build/Bada.app
#   make test       build and run the tests (the key table's cases, the transcripts, the unit tests)
#   make check      the built app's own check: the engine linked in, the settings folder as read
#   make install    copy it to ~/Library/Input Methods, register it, add a login agent
#   make dist       build/Bada-<version>.zip
#   make install-prebuilt PREBUILT=path/Bada.app   install an app built elsewhere, unbuilt
#   make uninstall  remove it (the settings folder stays)
#   make clean
ROOT       := ../..
ENGINE     := $(ROOT)/engine
HEADER     := $(ENGINE)/include/bada.h
BUILD      := build
APP        := $(BUILD)/Bada.app
BUNDLE_ID  := com.teammirae.inputmethod.Bada
# The engine is built into this folder's own target directory, so nothing outside harness/mac is written.
CARGO_DIR  := $(BUILD)/cargo
INSTALL_TO := $(HOME)/Library/Input Methods
LOGIN_AGENT := $(HOME)/Library/LaunchAgents/$(BUNDLE_ID).login.plist
MIN_MACOS  := 13.0
CORE       := $(wildcard Sources/Core/*.swift)
SOURCES    := $(CORE) $(wildcard Sources/App/*.swift)
CARGO      := $(shell command -v cargo || echo $(HOME)/.cargo/bin/cargo)
SIGN_IDENTITY ?= $(or $(shell security find-identity -v -p codesigning 2>/dev/null | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -1),-)
LSREGISTER := /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
# bada has one version for everything in the repository (the standard, §1).
VERSION    := $(shell cat $(ROOT)/VERSION)
DIST       := $(BUILD)/Bada-$(VERSION).zip
ICONS      := $(BUILD)/bada.tiff $(BUILD)/muhenkan.tiff $(BUILD)/henkan.tiff
# The engine's data (the standard, §11): each file is the buffer of its name. With their licenses.
DATA       := $(addprefix $(ENGINE)/data/,ko-words.tsv en-words.txt ja-words.txt ja-kana-native.txt ja-common-words.txt \
              kanji.bin hanja.tsv ko-jamo-4gram.bin ja-shortcuts.tsv symbols.tsv emoji.tsv unicode-names.tsv key-models.bin) \
              $(wildcard $(ENGINE)/data/LICENSE-*.txt)
# The Keyboard Viewer's legends, one keylayout for each stock layout that types (tools/make-keylayout.swift).
KEYLAYOUTS := $(BUILD)/keylayouts/.stamp
SWIFTFLAGS := -O -swift-version 5 -import-objc-header $(HEADER)
FRAMEWORKS := -framework Cocoa -framework InputMethodKit -framework Carbon -framework IOKit -framework CoreServices -framework WebKit
# The settings page (settings/), and the command line that hosts it in the settings window.
PAGE       := $(wildcard $(ROOT)/settings/src/*)
TESTS      := $(BUILD)/test/key-table-cases $(BUILD)/test/transcript $(BUILD)/test/unit

export MACOSX_DEPLOYMENT_TARGET := $(MIN_MACOS)

.PHONY: all test check install install-prebuilt dist uninstall clean FORCE
all: $(APP)
$(shell mkdir -p $(BUILD) && touch $(BUILD)/.metadata_never_index)

# The engine, as a static library for both architectures. Cargo decides what is out of date; the
# library here only changes when its bytes do, so nothing is relinked for nothing.
$(BUILD)/libbada.a: FORCE
	cd $(ROOT) && $(CARGO) build --release -p bada-engine --target aarch64-apple-darwin --target-dir "$(abspath $(CARGO_DIR))"
	cd $(ROOT) && $(CARGO) build --release -p bada-engine --target x86_64-apple-darwin --target-dir "$(abspath $(CARGO_DIR))"
	lipo -create -output $@.new $(CARGO_DIR)/aarch64-apple-darwin/release/libbada.a $(CARGO_DIR)/x86_64-apple-darwin/release/libbada.a
	@if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

# The command line, for both architectures: the settings window's host (SettingsWindow.swift). It
# is kept apart from Bada, the app's own program: a Mac's disk doesn't tell bada from Bada.
$(BUILD)/cli/bada: FORCE
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(CARGO) build --release -p bada-cli --target aarch64-apple-darwin --target-dir "$(abspath $(CARGO_DIR))"
	cd $(ROOT) && $(CARGO) build --release -p bada-cli --target x86_64-apple-darwin --target-dir "$(abspath $(CARGO_DIR))"
	lipo -create -output $@.new $(CARGO_DIR)/aarch64-apple-darwin/release/bada $(CARGO_DIR)/x86_64-apple-darwin/release/bada
	@if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

$(BUILD)/%/Bada: $(SOURCES) $(HEADER) $(BUILD)/libbada.a
	@mkdir -p $(dir $@)
	swiftc $(SWIFTFLAGS) -target $*-apple-macos$(MIN_MACOS) $(FRAMEWORKS) $(SOURCES) $(BUILD)/libbada.a -o $@

$(BUILD)/Bada: $(BUILD)/arm64/Bada $(BUILD)/x86_64/Bada
	lipo -create -output $@ $^

# The keylayouts are written by a tool that asks the engine what each key types.
$(BUILD)/make-keylayout: tools/make-keylayout.swift Sources/Core/KeyTable.swift Sources/Core/Reply.swift Sources/Core/Engine.swift Sources/App/KeyMap.swift $(HEADER) $(BUILD)/libbada.a
	@mkdir -p $(BUILD)/keylayout-tool
	cp tools/make-keylayout.swift $(BUILD)/keylayout-tool/main.swift
	swiftc $(SWIFTFLAGS) $(BUILD)/keylayout-tool/main.swift Sources/Core/KeyTable.swift Sources/Core/Reply.swift Sources/Core/Engine.swift Sources/App/KeyMap.swift $(BUILD)/libbada.a -o $@

$(KEYLAYOUTS): $(BUILD)/make-keylayout
	rm -rf $(BUILD)/keylayouts
	$(BUILD)/make-keylayout $(BUILD)/keylayouts
	touch $@

$(BUILD)/Info.plist: Info.plist.in $(ROOT)/VERSION $(KEYLAYOUTS)
	sed -e 's/@VERSION@/$(VERSION)/g' -e '/^@KLINFO@$$/r $(BUILD)/keylayouts/KLInfo.plist' -e '/^@KLINFO@$$/d' Info.plist.in > $@
	plutil -lint $@
	xmllint --noout $@
	@! grep -q '@' $@ || { echo "Info.plist still has a marker"; exit 1; }

$(BUILD)/%.tiff: Resources/icons/%.png Resources/icons/%@2x.png
	tiffutil -cathidpicheck Resources/icons/$*.png Resources/icons/$*@2x.png -out $@

# The code-signing identifier must equal the bundle id, or macOS refuses the input method's
# connection and nothing types (the standard, §12).
$(APP): $(BUILD)/Bada $(BUILD)/cli/bada $(ICONS) $(KEYLAYOUTS) $(BUILD)/Info.plist $(wildcard Resources/*.lproj/*) $(ROOT)/LICENSE $(DATA) $(PAGE)
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Helpers $@/Contents/Resources/data $@/Contents/Resources/settings
	cp $(DATA) $@/Contents/Resources/data/
	cp $(PAGE) $@/Contents/Resources/settings/
	cp $(BUILD)/Info.plist $@/Contents/Info.plist
	cp $(BUILD)/Bada $@/Contents/MacOS/Bada
	cp $(BUILD)/cli/bada $@/Contents/Helpers/bada
	codesign --force --sign "$(SIGN_IDENTITY)" --identifier com.teammirae.bada.cli $@/Contents/Helpers/bada
	cp $(ICONS) $(BUILD)/keylayouts/*.keylayout $@/Contents/Resources/
	cp -R Resources/en.lproj Resources/ko.lproj Resources/ja.lproj $@/Contents/Resources/
	cp $(ROOT)/LICENSE $@/Contents/Resources/LICENSE.txt
	codesign --force --sign "$(SIGN_IDENTITY)" --identifier $(BUNDLE_ID) $@
	@codesign -dv $@ 2>&1 | grep -q '^Identifier=$(BUNDLE_ID)$$' || { echo "the code-signing identifier isn't $(BUNDLE_ID)"; exit 1; }

# Tests. They run on this machine's own architecture, and install nothing.
$(BUILD)/test/key-table-cases: Sources/Core/KeyTable.swift Tests/KeyTableCases/main.swift
	@mkdir -p $(dir $@)
	swiftc -O -swift-version 5 $^ -o $@

$(BUILD)/test/transcript: Sources/Core/KeyTable.swift Sources/Core/Reply.swift Sources/Core/Engine.swift Sources/Core/ReplyApplier.swift Tests/Transcript/TextFieldModel.swift Tests/Transcript/main.swift $(HEADER) $(BUILD)/libbada.a
	@mkdir -p $(dir $@)
	swiftc $(SWIFTFLAGS) $(filter %.swift,$^) $(BUILD)/libbada.a -o $@

$(BUILD)/test/unit: $(CORE) Tests/Transcript/TextFieldModel.swift Tests/Unit/main.swift $(HEADER) $(BUILD)/libbada.a
	@mkdir -p $(dir $@)
	swiftc $(SWIFTFLAGS) -framework CoreServices $(filter %.swift,$^) $(BUILD)/libbada.a -o $@

test: $(TESTS)
	$(BUILD)/test/key-table-cases $(ROOT)/docs/cases/key-table.json
	$(BUILD)/test/transcript $(ROOT)/tools/conformance/transcripts/bada.jsonl $(ROOT)/tools/conformance/transcripts/any-engine.jsonl
	$(BUILD)/test/unit $(ROOT) $(BUILD)/test/settings-folder
	@! grep -rnE 'me\.sjun|BadaIME|Application Support/bada-ime' Sources tools Resources Info.plist.in || { echo "an id, file or folder of bada ime is named: bada shares none (the standard, §12)"; exit 1; }

# The built app's own check, on an empty settings folder: nothing is installed, and nothing of the
# user's is read or written.
check: $(APP)
	@rm -rf $(BUILD)/check-home && mkdir -p $(BUILD)/check-home
	BADA_HOME="$(abspath $(BUILD)/check-home)" $(APP)/Contents/MacOS/Bada --check

install: $(APP)
	$(MAKE) install-prebuilt PREBUILT=$(APP)

# No dependencies: installs PREBUILT as it is (an app from CI, or the one `install` just built).
PREBUILT ?= $(APP)
install-prebuilt:
	@test -d "$(PREBUILT)" || { echo "No app at $(PREBUILT)"; exit 1; }
	@killall Bada 2>/dev/null || true
	mkdir -p "$(INSTALL_TO)"
	rm -rf "$(INSTALL_TO)/.Bada.app.new"
	ditto "$(PREBUILT)" "$(INSTALL_TO)/.Bada.app.new"
	xattr -dr com.apple.quarantine "$(INSTALL_TO)/.Bada.app.new" 2>/dev/null || true
	if [ -d "$(INSTALL_TO)/Bada.app" ]; then rm -rf "$(INSTALL_TO)/.Bada.app.old"; mv "$(INSTALL_TO)/Bada.app" "$(INSTALL_TO)/.Bada.app.old"; fi
	mv "$(INSTALL_TO)/.Bada.app.new" "$(INSTALL_TO)/Bada.app"
	rm -rf "$(INSTALL_TO)/.Bada.app.old"
	$(LSREGISTER) -u "$(abspath $(PREBUILT))" 2>/dev/null || true
	$(LSREGISTER) -f "$(INSTALL_TO)/Bada.app"
	swift tools/register.swift "$(INSTALL_TO)/Bada.app"
	mkdir -p "$(dir $(LOGIN_AGENT))"
	cp tools/login-agent.plist "$(LOGIN_AGENT)"

# --norsrc: no extended attributes, which a zip carries as ._ files; unzip extracts those into
# the bundle and its seal breaks (the keylayouts carry com.apple.TextEncoding).
dist: $(APP)
	rm -f $(DIST)
	ditto -c -k --norsrc --keepParent $(APP) $(DIST)
	@echo $(DIST)

uninstall:
	@killall Bada 2>/dev/null || true
	rm -rf "$(INSTALL_TO)/Bada.app" "$(LOGIN_AGENT)"

clean:
	rm -rf $(BUILD)
