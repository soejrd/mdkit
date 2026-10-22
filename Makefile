# SPDX-License-Identifier: CC0-1.0
# Build convention for standalone voices. A worker adds src/<family>/<voice>/
# with mdkit_<family>_<voice>.{c,h} and voice.mk; no root-file edit is needed.

CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
WASM_CC ?= $(or $(wildcard /opt/homebrew/opt/llvm/bin/clang),$(shell command -v clang))
WASM_LD ?= wasm-ld

INCLUDES := -Iinclude
VOICES := $(sort $(patsubst src/%/voice.mk,%,$(wildcard src/*/*/voice.mk)))
VOICE_KEYS := $(subst /,_,$(VOICES))

# The root build learns each voice's identity from its own voice.mk.
include $(wildcard src/*/*/voice.mk)

OUT := out
NATIVE_BINS :=
TEST_BINS :=
WASM_BINS :=
ifeq ($(strip $(VOICES)),)
$(error no voices found under src/*/*/voice.mk)
endif

define VOICE_RULES
$(OUT)/$(2)/voice.o: src/$(1)/mdkit_$(2).c src/$(1)/mdkit_$(2).h src/$(1)/voice.mk include/mdkit.h include/mdkit_voice.h
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(INCLUDES) -Isrc/$(1) -c $$< -o $$@

$(OUT)/$(2)/test_voice: $(or $($(2)_VOICE_TEST),tests/test_voice.c) $(OUT)/$(2)/voice.o src/$(1)/mdkit_$(2).h
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(INCLUDES) -Isrc/$(1) \
	  -DMDKIT_TEST_VOICE_HEADER='"mdkit_$(2).h"' \
	  -DVOICE_SYMBOL=$($(2)_VOICE_SYMBOL) -DVOICE_MACHINE_ID=$($(2)_VOICE_ID) \
	  $$< $(OUT)/$(2)/voice.o -lm -o $$@

$(OUT)/$(2)/render_voice: examples/render_voice.c $(OUT)/$(2)/voice.o src/$(1)/mdkit_$(2).h
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(INCLUDES) -Isrc/$(1) \
	  -DMDKIT_EXAMPLE_VOICE_HEADER='"mdkit_$(2).h"' \
	  -DVOICE_SYMBOL=$($(2)_VOICE_SYMBOL) \
	  $$< $(OUT)/$(2)/voice.o -lm -o $$@

$(OUT)/$(2)/voice.wasm: website/wasm_bridge.c src/$(1)/mdkit_$(2).c src/$(1)/mdkit_$(2).h include/mdkit.h include/mdkit_voice.h
	@mkdir -p $$(@D)
	$$(WASM_CC) --target=wasm32 -std=c99 -O2 -nostdlib -ffreestanding \
	  -Wl,--no-entry -Wl,--strip-all \
	  $(addprefix -Wl$(comma)--export=,mdkit_$(2)_state_size mdkit_$(2)_init \
	    mdkit_$(2)_set_control mdkit_$(2)_trigger mdkit_$(2)_render mdkit_$(2)_sample_rate) \
	  $$(INCLUDES) -Isrc/$(1) -DMDKIT_EXAMPLE_VOICE_HEADER='"mdkit_$(2).h"' \
	  -DVOICE_SYMBOL=$($(2)_VOICE_SYMBOL) -DVOICE_PREFIX=mdkit_$(2) \
	  website/wasm_bridge.c src/$(1)/mdkit_$(2).c -o $$@

.PHONY: check-$(subst /,-,$(1))
check-$(subst /,-,$(1)): $(OUT)/$(2)/test_voice
	$$<

NATIVE_BINS += $(OUT)/$(2)/voice.o $(OUT)/$(2)/render_voice
TEST_BINS += $(OUT)/$(2)/test_voice
LINK_OBJECTS += $(OUT)/$(2)/voice.o
WASM_BINS += $(OUT)/$(2)/voice.wasm
endef

comma := ,
$(foreach v,$(VOICES),$(eval $(call VOICE_RULES,$(v),$(subst /,_,$(v)))))

.PHONY: all native check test wasm family family-check clean
all: native check wasm web check-web
native: $(NATIVE_BINS)
$(OUT)/link_all: tests/link_all.c $(LINK_OBJECTS) $(OUT)/catalog.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(OUT) tests/link_all.c $(LINK_OBJECTS) -lm -o $@

family: $(OUT)/link_all
	$<

PI_HEADER_CHECK := $(if $(filter pi_%,$(VOICE_KEYS)),$(OUT)/pi_headers)
check: $(TEST_BINS) $(OUT)/link_all $(OUT)/efm_defaults $(OUT)/default_settings $(PI_HEADER_CHECK)
	@set -e; for t in $(TEST_BINS); do $$t; done
	$(OUT)/link_all
	$(OUT)/efm_defaults
	$(OUT)/default_settings
	$(if $(PI_HEADER_CHECK),$(PI_HEADER_CHECK),@:)

test: check
wasm: $(WASM_BINS)
	@set -e; for w in $(WASM_BINS); do node tests/check_wasm.cjs $$w; done
clean:
	rm -rf $(OUT) website/mdkit.wasm

VOICE_SOURCES := $(foreach v,$(VOICES),src/$(v)/mdkit_$(subst /,_,$(v)).c)
VOICE_HEADERS := $(foreach v,$(VOICES),src/$(v)/mdkit_$(subst /,_,$(v)).h)
WEB_EXPORTS := machine_count machine_name machine_id control_count control_name default_control init set_control set_level trigger render capacity native_sample_rate runtime_error
.PHONY: catalog web check-web serve ready FORCE
FORCE:
$(OUT)/catalog.h: FORCE tools/generate_catalog.py $(wildcard src/*/*/voice.mk)
	python3 tools/generate_catalog.py $@

website/mdkit.wasm: $(OUT)/catalog.h website/player_bridge.c $(VOICE_SOURCES) $(VOICE_HEADERS) include/mdkit.h include/mdkit_voice.h
	$(WASM_CC) --target=wasm32 -std=c99 -O2 -nostdlib -ffreestanding \
	  -Wl,--no-entry -Wl,--strip-all -Wl,-z,stack-size=262144 \
	  $(addprefix -Wl$(comma)--export=mdkit_web_,$(WEB_EXPORTS)) \
	  $(INCLUDES) -I$(OUT) website/player_bridge.c $(VOICE_SOURCES) -o $@

web: website/mdkit.wasm
check-web: web
	node tests/web/player.cjs
serve: web
	python3 -m http.server 8000 --directory website
ready: all
	python3 tools/check_package.py --require-all

$(OUT)/efm_defaults: tests/efm/defaults.c $(foreach v,$(filter efm_%,$(VOICE_KEYS)),$(OUT)/$(v)/voice.o) include/mdkit.h
	$(CC) $(CFLAGS) $(INCLUDES) $< $(foreach v,$(filter efm_%,$(VOICE_KEYS)),$(OUT)/$(v)/voice.o) -lm -o $@

$(OUT)/default_settings: tests/default_settings.c $(LINK_OBJECTS) $(OUT)/catalog.h include/mdkit.h
	$(CC) $(CFLAGS) $(INCLUDES) -I$(OUT) $< $(LINK_OBJECTS) -lm -o $@

$(OUT)/pi_headers: tests/pi/headers.c $(wildcard src/pi/*/*.h) include/mdkit.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@

.PHONY: check-pi
check-pi:
	python3 tests/pi/run.py
