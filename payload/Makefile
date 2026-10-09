# Builds the payloads with the template's pinned Payload SDK (run `make deps` at
# the repository root first):
#  - spotify_mdns_helper.elf: announces the title app's Zeroconf port over mDNS.
#  - spotify_speaker.elf: the whole speaker as a payload (cspot + live WAV stream
#    for the web app tile). Needs tools/build-cspot-payload.sh (see howitworks.md).
#  - webapp_install.elf / webapp_uninstall.elf: install or remove the "PS5 Speaker"
#    web app tile (probes/webapp_install.c).
# SPDX-License-Identifier: GPL-3.0-or-later

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
PS5_PAYLOAD_SDK ?= $(ROOT)/.deps/native/ps5-payload-sdk
CSPOT := $(ROOT)/third_party/cspot/cspot
BELL := $(CSPOT)/bell
MDNSSVC := $(BELL)/external/mdnssvc
DEPS := $(ROOT)/build/payload-deps

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

HELPER := $(ROOT)/build/payload/spotify_mdns_helper.elf
SPEAKER := $(ROOT)/build/payload/spotify_speaker.elf
INSTALL := $(ROOT)/build/payload/webapp_install.elf
UNINSTALL := $(ROOT)/build/payload/webapp_uninstall.elf
TILE_ICON := $(ROOT)/sce_sys/icon0.png
CFLAGS := -O2 -Wall -I$(MDNSSVC)

SPEAKER_DEFINES := -DBELL_DISABLE_CODECS -DFMT_HEADER_ONLY -DPB_ENABLE_MALLOC -DPB_FIELD_32BIT
SPEAKER_INCLUDES := -I$(CSPOT)/include -I$(DEPS)/cspot-build -I$(DEPS)/cspot-build/bell \
	-I$(BELL)/main/io/include -I$(BELL)/main/utilities/include -I$(BELL)/main/platform \
	-I$(BELL)/main/audio-codec/include -I$(BELL)/main/audio-dsp/include \
	-I$(BELL)/main/audio-sinks/include -I$(BELL)/external/civetweb/include \
	-isystem $(BELL)/external/nanopb -isystem $(BELL)/external/tremor \
	-isystem $(BELL)/external/cJSON -isystem $(BELL)/external/fmt/include \
	-isystem $(MDNSSVC) -isystem $(BELL)/external/nlohmann_json/include \
	-isystem $(DEPS)/prefix/include
SPEAKER_LIBS := $(DEPS)/cspot-build/libcspot.a $(DEPS)/cspot-build/bell/libbell.a \
	$(DEPS)/prefix/lib/libmbedtls.a $(DEPS)/prefix/lib/libmbedx509.a \
	$(DEPS)/prefix/lib/libmbedcrypto.a -lpthread

all: $(HELPER) $(SPEAKER) $(INSTALL) $(UNINSTALL)

$(INSTALL): $(ROOT)/probes/webapp_install.c $(TILE_ICON)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -DICON0_PATH='"$(TILE_ICON)"' -o $@ $< -ldl

$(UNINSTALL): $(ROOT)/probes/webapp_install.c $(TILE_ICON)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -DUNINSTALL -DICON0_PATH='"$(TILE_ICON)"' -o $@ $< -ldl

$(HELPER): mdns_helper.c $(MDNSSVC)/mdns.c $(MDNSSVC)/mdnsd.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

$(SPEAKER): spotify_daemon.cpp $(DEPS)/cspot-build/libcspot.a
	@mkdir -p $(dir $@)
	$(CXX) -O2 -Wall -std=c++17 -fexceptions $(SPEAKER_DEFINES) $(SPEAKER_INCLUDES) \
		-o $@ spotify_daemon.cpp $(SPEAKER_LIBS) -ldl

clean:
	rm -f $(HELPER) $(SPEAKER) $(INSTALL) $(UNINSTALL)

.PHONY: all clean
