#!/usr/bin/env python3
"""Compile the actual C modules with small IDF mocks; no ESP32 needed."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
headers = ['esp_err.h', 'esp_log.h', 'esp_check.h', 'esp_http_server.h',
           'sdkconfig.h', 'nvs.h', 'esp_littlefs.h', 'freertos/FreeRTOS.h',
           'freertos/task.h', 'freertos/semphr.h', 'driver/gpio.h', 'driver/i2c_master.h']
with tempfile.TemporaryDirectory(prefix='programmer-tests-') as tmp:
    tmp = Path(tmp)
    for name in headers:
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "mock_idf.h"\n')
    for name in ['programmer', 'front_panel']:
        out = tmp / name
        cmd = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-variable',
               '-Wno-format-truncation', '-g', '-fsanitize=address,undefined',
               '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections',
               '-I' + str(tmp), '-I' + str(root / 'tests'), '-I' + str(root / 'main'),
               '-include', str(root / 'tests/mock_idf.h'),
               str(root / ('tests/test_' + name + '.c'))]
        if name == 'programmer':
            cmd += [str(root / 'main/storage.c'), '-Wl,--wrap=storage_exists',
                    '-Wl,--wrap=storage_delete']
        cmd += ['-Wl,--gc-sections', '-o', str(out)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(out)], check=True)
