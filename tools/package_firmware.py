#!/usr/bin/env python3
"""Create a local, checksummed USB firmware package; never includes NVS or keys."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

def package(build,output,profile):
    build,output=Path(build).resolve(),Path(output).resolve()
    chip='esp32c3' if profile=='c3' else 'esp32s3'
    app='smart_plant_node.bin' if profile=='c3' else 'smart_plant_hub.bin'
    files={'app':build/app,'bootloader':build/'bootloader/bootloader.bin','partition_table':build/'partition_table/partition-table.bin'}
    config=json.loads((build/'config/sdkconfig.json').read_text(encoding="utf-8"))
    if config.get('IDF_TARGET')!=chip:raise ValueError('Build target does not match profile')
    if profile!='c3':
        expected='8MB' if profile=='n8r2' else '16MB'
        mode='SPIRAM_MODE_QUAD' if profile=='n8r2' else 'SPIRAM_MODE_OCT'
        if config.get('ESPTOOLPY_FLASHSIZE')!=expected or not config.get(mode):raise ValueError('Build flash/PSRAM settings do not match profile')
    if not all(p.is_file() for p in files.values()):raise ValueError('Build is incomplete')
    allowed = {p.name for p in files.values()} | {'manifest.json','nvs_partition_gen.py','ESP-IDF-LICENSE'}
    if output.exists() and any(p.name not in allowed or not p.is_file() or p.is_symlink() for p in output.iterdir()):
        raise ValueError('Output directory contains unrelated files; use a clean firmware package directory')
    output.mkdir(parents=True,exist_ok=True)
    manifest={'format':1,'profile':profile,'chip':chip,'app_offset':0x30000}
    for key,src in files.items():
        dst=output/src.name;shutil.copyfile(src,dst)
        manifest[key]={'file':dst.name,'sha256':hashlib.sha256(dst.read_bytes()).hexdigest()}
    sdk=Path(os.environ.get('IDF_PATH',''))
    generator=sdk/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'
    if generator.is_file():
        shutil.copyfile(generator,output/'nvs_partition_gen.py')
        if (sdk/'LICENSE').is_file():shutil.copyfile(sdk/'LICENSE',output/'ESP-IDF-LICENSE')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return output

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',required=True);p.add_argument('--output',required=True);p.add_argument('--profile',required=True,choices=['c3','n8r2','n16r8']);a=p.parse_args()
    print(package(a.build,a.output,a.profile))
