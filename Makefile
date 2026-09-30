# pw-mpris-visualcard / native -- single-process C++ renderer
# All dependencies are distribution system libraries; no third-party package manager involved.

CXX      ?= g++
PKGS     := cairo pangocairo fontconfig libpipewire-0.3 sdbus-c++ libcurl gdk-pixbuf-2.0 glib-2.0
# -O3 -march=native pays off clearly on the rotation hot loop (measured -22% per frame).
# The cost is a binary bound to the local instruction set; to run elsewhere or distribute it,
# use make PORTABLE=1.
ifeq ($(PORTABLE),1)
  ARCHFLAGS :=
else
  ARCHFLAGS := -march=native
endif

CXXFLAGS ?= -O3 -g -funroll-loops $(ARCHFLAGS)
CXXFLAGS += -std=c++20 -Wall -Wextra $(EXTRA_CXXFLAGS) $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

TARGET   := pw-mpris-visualcard-native
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:.cpp=.o)

# The video node output is a separate library, pulled in as a git submodule (see "Build from
# source" in the README).
# Its sources are compiled straight into this project's build tree: one set of compile flags, no
# ABI to track, and no .o files left behind in the submodule directory.
PWNODE_DIR  ?= lib/pw-video-simple-interface
PWNODE_SRC  := $(wildcard $(PWNODE_DIR)/src/*.cpp) $(wildcard $(PWNODE_DIR)/extras/*.cpp)
PWNODE_OBJ  := $(patsubst $(PWNODE_DIR)/%.cpp,build/pwvideo/%.o,$(PWNODE_SRC))
CXXFLAGS    += -I$(PWNODE_DIR)/src -I$(PWNODE_DIR)/extras
OBJ         += $(PWNODE_OBJ)
DEP         := $(OBJ:.o=.d)

ifeq ($(PWNODE_SRC),)
$(error Missing submodule $(PWNODE_DIR): run git submodule update --init --recursive first)
endif

# systemd user service: the unit is a template; @REPO@ / @ARGS@ are substituted at install time
UNIT         := pw-mpris-visualcard.service
UNIT_DIR     ?= $(HOME)/.config/systemd/user
SERVICE_ARGS ?= --node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3

.PHONY: all clean dump run install-service uninstall-service

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build/pwvideo/%.o: $(PWNODE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# Publish no video; render a single PNG, used to tune the layout
dump: $(TARGET)
	./$(TARGET) --demo --lyrics 4 --time 1 --album 1 --dump /tmp/card.png

run: $(TARGET)
	./$(TARGET)

# Render the unit and install it into the user systemd directory; does not enable/start it, so
# the machine's existing state is left unchanged
install-service: $(TARGET)
	@mkdir -p $(UNIT_DIR)
	sed -e 's|@REPO@|$(CURDIR)|g' -e 's|@ARGS@|$(SERVICE_ARGS)|g' \
	    $(UNIT) > $(UNIT_DIR)/$(UNIT)
	systemctl --user daemon-reload
	@echo "Installed $(UNIT_DIR)/$(UNIT)"
	@echo "To enable and start: systemctl --user enable --now pw-mpris-visualcard"
	@echo "After changing arguments: systemctl --user restart pw-mpris-visualcard"

uninstall-service:
	-systemctl --user disable --now pw-mpris-visualcard
	rm -f $(UNIT_DIR)/$(UNIT)
	systemctl --user daemon-reload
	@echo "Uninstalled $(UNIT_DIR)/$(UNIT)"

clean:
	rm -f $(OBJ) $(DEP) $(TARGET)
	rm -rf build

-include $(DEP)
