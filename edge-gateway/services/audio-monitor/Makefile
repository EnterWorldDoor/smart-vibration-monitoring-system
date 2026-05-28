#
# audio-monitor — Makefile
# Pattern mirrored from vision-service Makefile.
#

VENV       := venv
PYTHON     := $(VENV)/bin/python
PIP        := $(VENV)/bin/pip

SRCDIR     := src
TESTDIR    := tests
SYSTEMDDIR := systemd

.PHONY: all venv install test clean install-service help

all: venv

venv: requirements.txt
	python3 -m venv $(VENV)
	$(PIP) install --upgrade pip
	$(PIP) install -r requirements.txt
	@echo "venv created and dependencies installed"
	@echo "NOTE: libportaudio2 must be installed via apt:"
	@echo "  sudo apt install -y libportaudio2"

install: venv
	@echo "For systemd installation run: sudo make install-service"

install-service: venv
	install -d /opt/edge-gateway/services/audio-monitor
	cp -r $(SRCDIR) $(TESTDIR) requirements.txt Makefile /opt/edge-gateway/services/audio-monitor/
	$(MAKE) -C /opt/edge-gateway/services/audio-monitor venv
	install -d /etc/systemd/system
	install -m 644 $(SYSTEMDDIR)/audio-monitor.service /etc/systemd/system/
	systemctl daemon-reload
	@echo ""
	@echo "To enable and start:"
	@echo "  sudo systemctl enable audio-monitor"
	@echo "  sudo systemctl start audio-monitor"

test: venv
	PYTHONPATH=. $(PYTHON) -m pytest $(TESTDIR) -v

test-unit: venv
	PYTHONPATH=. $(PYTHON) -m pytest \
		$(TESTDIR)/test_config.py \
		$(TESTDIR)/test_processor.py \
		$(TESTDIR)/test_detector.py \
		$(TESTDIR)/test_file_store.py \
		-v

test-mock: venv
	PYTHONPATH=. $(PYTHON) -m pytest \
		$(TESTDIR)/test_capture.py \
		$(TESTDIR)/test_db_client.py \
		-v

test-integration: venv
	PYTHONPATH=. $(PYTHON) -m pytest \
		$(TESTDIR)/test_end_to_end.py \
		-v

clean:
	rm -rf $(VENV) __pycache__
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true

help:
	@echo "make                      — Create venv and install dependencies"
	@echo "make test                 — Run all tests"
	@echo "make test-unit            — Run config + processor + detector + file_store tests"
	@echo "make test-mock            — Run capture mock + db mock tests"
	@echo "make test-integration     — Run end-to-end test"
	@echo "make clean                — Remove venv and __pycache__"
	@echo "make install              — Show install instructions"
	@echo "make install-service      — Install venv + systemd unit to system"
