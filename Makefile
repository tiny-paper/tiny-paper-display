test: test-firmware test-python test-site   ## everything that needs no hardware

test-firmware:
	$(MAKE) -C firmware test

VENV := build/venv
$(VENV)/bin/pytest: python/pyproject.toml
	python3 -m venv $(VENV)
	$(VENV)/bin/pip -q install -e "./python[test]"

test-python: $(VENV)/bin/pytest
	$(VENV)/bin/python -m pytest -q python/tests

test-site:
	node --test site/test/*.test.mjs

.PHONY: test test-firmware test-python test-site
