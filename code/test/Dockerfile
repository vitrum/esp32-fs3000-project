FROM espressif/idf:v4.4.6

RUN apt-get update \
    && apt-get install -y --no-install-recommends python3-venv \
    && rm -rf /var/lib/apt/lists/*

COPY test/requirements.txt /opt/aeroprobe-test-requirements.txt
RUN python -m venv /opt/pytest-venv \
    && /opt/pytest-venv/bin/pip install --no-cache-dir -r /opt/aeroprobe-test-requirements.txt

WORKDIR /project