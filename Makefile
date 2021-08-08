# https://github.com/arduino/arduino-cli/releases

port := $(shell python3 board_detect.py 2>/dev/null)
fqbn=esp32:esp32:esp32
src=xvc-esp32

default:
	arduino-cli compile --warnings=all --fqbn="${fqbn}" "${src}"

upload:
	@# echo $(port)
	arduino-cli compile --fqbn="${fqbn}" "${src}"
	arduino-cli -v upload -p "${port}" --fqbn="${fqbn}" "${src}"

install_platform:
	arduino-cli core update-index --config-file arduino-cli.yaml
	arduino-cli core install esp32:esp32

deps:
	pip3 install pyserial
	# arduino-cli lib install "Etherkit Si5351"

install_arduino_cli:
	curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh

format:
	astyle --options="formatter.conf" xvc-esp32/xvc-esp32.ino
