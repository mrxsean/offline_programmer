#!/usr/bin/env python3
"""Compile the actual C modules with small IDF mocks; no ESP32 needed."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
headers = ['esp_err.h', 'esp_log.h', 'esp_check.h', 'esp_http_server.h',
           'sdkconfig.h', 'nvs.h', 'esp_littlefs.h', 'esp_ota_ops.h', 'esp_app_desc.h',
           'esp_app_format.h', 'esp_system.h', 'freertos/FreeRTOS.h',
           'freertos/task.h', 'freertos/semphr.h', 'driver/gpio.h', 'driver/i2c_master.h',
           'esp_wifi.h', 'esp_event.h', 'esp_netif.h', 'esp_mac.h']
with tempfile.TemporaryDirectory(prefix='programmer-tests-') as tmp:
    tmp = Path(tmp)
    for name in headers:
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "mock_idf.h"\n')
    macos = sys.platform == 'darwin'
    for name in ['programmer', 'front_panel', 'wifi_ap']:
        out = tmp / name
        cmd = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-variable',
               '-g', '-fsanitize=address,undefined',
               '-I' + str(tmp), '-I' + str(root / 'tests'), '-I' + str(root / 'main'),
               '-include', str(root / 'tests/mock_idf.h'),
               str(root / ('tests/test_' + name + '.c'))]
        if macos:
            if name == 'programmer':
                # Mach-O has no --wrap: rename storage definitions in a temporary copy.
                storage = (root / 'main/storage.c').read_text()
                test = (root / 'tests/test_programmer.c').read_text()
                for symbol in ['storage_exists', 'storage_delete', 'storage_list']:
                    storage = storage.replace(symbol + '(', 'real_' + symbol + '(')
                    test = test.replace('__wrap_' + symbol, symbol)
                test = test.replace('../main/http_server.c', str(root / 'main/http_server.c'))
                (tmp / 'storage.c').write_text(storage)
                (tmp / 'test_programmer.c').write_text(test)
                cmd[-1] = str(tmp / 'test_programmer.c')
                cmd += [str(tmp / 'storage.c')]
            cmd += ['-Wl,-dead_strip']
        else:
            cmd += ['-Wno-format-truncation', '-fno-pie', '-no-pie',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
        if name == 'programmer' and not macos:
            cmd += [str(root / 'main/storage.c'), '-Wl,--wrap=storage_exists',
                    '-Wl,--wrap=storage_delete', '-Wl,--wrap=storage_list']
        cmd += ['-o', str(out)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(out)], check=True)
