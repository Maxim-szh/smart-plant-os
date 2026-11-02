#!/usr/bin/env python3
"""Smart Plant OS USB assistant. No erase command and no NVS writes in update mode."""
import argparse
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import uuid
import secrets
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
PROFILES = {'n8r2': ('esp32s3', 9, 8), 'n16r8': ('esp32s3', 9, 16), 'c3': ('esp32c3', 5, 4)}

def run(args, capture=False):
    return subprocess.run(list(map(str,args)),check=True,text=True,capture_output=capture,cwd=ROOT)

def tool(*args, capture=False):
    if '--baud' not in args:args=('--baud','921600',*args)
    return run([sys.executable,'-m','esptool',*args],capture=capture)

def ports():
    try:
        from serial.tools.list_ports import comports
    except ImportError as e:
        raise ValueError('Установите зависимости: python -m pip install -r tools/requirements-flash.txt') from e
    return sorted(comports(),key=lambda p:p.device)

def choose_port():
    found = ports()
    for i,p in enumerate(found,1): print(f'{i}. {p.device} — {p.description}')
    if not found: raise ValueError('USB-порты не найдены. Подключите плату кабелем с передачей данных.')
    selected=input('Номер порта: ').strip()
    if not selected.isdigit() or not 1 <= int(selected) <= len(found): raise ValueError('Неверный номер порта')
    return found[int(selected)-1].device

def detect(port,profile,keep_rom=False):
    chip,_,mb=PROFILES[profile]
    after=('--after','no_reset') if keep_rom else ()
    result=tool('--chip',chip,'--port',port,*after,'flash_id',capture=True).stdout
    if f'{mb}MB' not in result: raise ValueError(f'Размер flash не подтверждён как {mb}MB для {profile}. Проверьте маркировку платы.')
    result=tool('--chip',chip,'--port',port,*after,'read_mac',capture=True).stdout
    match=re.search(r'^MAC:\s*([0-9a-fA-F:]{17})',result,re.M)
    if not match: raise ValueError('Не удалось определить MAC платы')
    return match.group(1).lower()

def parse_partitions(data):
    """Validate ESP-IDF partition table, including its MD5 marker."""
    apps=[]; checked=False; intervals=[]; records=[]
    for off in range(0,min(len(data),0x1000),32):
        row=data[off:off+32]
        if len(row)!=32: raise ValueError('Обрезанная таблица разделов')
        magic=struct.unpack_from('<H',row)[0]
        if magic==0xEBEB:
            if hashlib.md5(data[:off]).digest()!=row[16:32]: raise ValueError('Повреждена таблица разделов (MD5)')
            checked=True;break
        if magic!=0x50AA: raise ValueError('Таблица разделов отсутствует или не поддерживается')
        _,kind,subtype,address,size,label,flags=struct.unpack('<HBBLL16sL',row)
        if not size or address<0x9000 or address+size>0x1000000: raise ValueError('Некорректные границы раздела')
        if flags: raise ValueError('Зашифрованный/специальный раздел требует отдельного инструмента обновления')
        if any(address<b and address+size>a for a,b in intervals): raise ValueError('Разделы перекрываются')
        intervals.append((address,address+size))
        records.append((kind,subtype,address,size,label.rstrip(b"\0")))
        if kind==0: apps.append((subtype,address,size,label.rstrip(b'\0')))
    if not checked: raise ValueError('У таблицы разделов нет проверяемого MD5')
    if (1,2,0x9000,0x20000,b'nvs') not in records or (1,1,0x29000,0x1000,b'phy_init') not in records:raise ValueError('Не совпадает защищённая область NVS/PHY')
    if len(apps)==1 and apps[0][0]==0 and apps[0][1]==0x30000:return apps[0][1],apps[0][2]
    if (1,0,0x2a000,8192,b'otadata') in records and len(apps)==2 and apps[0][0]==16 and apps[1][0]==17 and apps[0][1]==0x30000 and apps[0][2]==apps[1][2] and apps[1][1]==apps[0][1]+apps[0][2] and apps[0][2] in (0x200000,0x1e0000):return apps[0][1],apps[0][2]
    raise ValueError('Неизвестная разметка приложений; запись отменена')

def load_bundle(directory,profile):
    directory=Path(directory).resolve()
    manifest=json.loads((directory/'manifest.json').read_text(encoding="utf-8"))
    chip,chip_id,mb=PROFILES[profile]
    if manifest.get('format') not in (1,2) or manifest.get('profile')!=profile or manifest.get('chip')!=chip: raise ValueError('Пакет не соответствует профилю платы')
    result={}
    for name in (('app','bootloader','partition_table','otadata') if manifest.get('format')==2 else ('app','bootloader','partition_table')):
        entry=manifest.get(name,{})
        file=entry.get('file','')
        if not file or Path(file).name!=file: raise ValueError('Некорректный путь файла в пакете')
        path=directory/file
        if path.is_symlink() or path.parent!=directory: raise ValueError('Ссылки в пакете не поддерживаются')
        raw=path.read_bytes()
        if hashlib.sha256(raw).hexdigest()!=entry.get('sha256'): raise ValueError(f'Не совпадает SHA-256 файла {file}')
        result[name]=path
    app=result['app'].read_bytes()
    if len(app)<24 or app[0]!=0xe9 or struct.unpack_from('<H',app,12)[0]!=chip_id: raise ValueError('Образ приложения предназначен другому чипу')
    if (1 << (app[3] >> 4))!=mb: raise ValueError('Размер flash в образе не соответствует профилю')
    if manifest.get('app_offset')!=0x30000: raise ValueError('Некорректное смещение приложения')
    offset,size=parse_partitions(result['partition_table'].read_bytes())
    if len(app)>size: raise ValueError('Приложение не помещается в раздел')
    if manifest.get('format')==2:
        if manifest.get('layout')!='ota-v1' or manifest.get('data_abi')!=1 or manifest['app'].get('size')!=len(app) or len(app)<128 or struct.unpack_from('<I',app,32)[0]!=0xabcd5432:raise ValueError('Несовместимый OTA-пакет')
        version=app[48:80].split(b'\0',1)[0].decode('ascii')
        project=app[80:112].split(b'\0',1)[0].decode('ascii')
        if version!=manifest.get('version') or not re.fullmatch(r'\d+\.\d+\.\d+-abi1',version) or project!=('smart_plant_node' if profile=='c3' else 'smart_plant_hub'):raise ValueError('Неверная версия или проект приложения')
        if len(result['otadata'].read_bytes())!=8192:raise ValueError('Некорректный otadata')
        if size!=(0x1e0000 if profile=='c3' else 0x200000):raise ValueError('Некорректный размер OTA-слотов')
    return result

def initial_ota_args(files):
    return ('0x2a000',files['otadata']) if 'otadata' in files else ()

def read_region(port,chip,address,size,path,keep_rom=False):
    after=('--after','no_reset') if keep_rom else ()
    tool('--chip',chip,'--port',port,'--baud','921600',*after,'read_flash',hex(address),hex(size),path,capture=True)

def require_blank_nvs(port,chip,keep_rom=False):
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'nvs.bin'
        if keep_rom:read_region(port,chip,0x9000,0x20000,path,keep_rom=True)
        else:read_region(port,chip,0x9000,0x20000,path)
        if any(b!=255 for b in path.read_bytes()): raise ValueError('NVS уже содержит данные. Для действующей платы используйте обновление. Этот мастер не стирает NVS и не переустанавливает существующие ключи.')

def source_build(profile):
    sys.path.insert(0,str(ROOT/'tools'))
    import install
    _,build=install.build('node' if profile=='c3' else 'hub',PROFILES[profile][0],None if profile=='c3' else profile)
    from package_firmware import package
    directory=build/'usb-package';package(build,directory,profile)
    return directory

def nvs_generate(csv_path,bin_path,bundle):
    run([sys.executable,'-m','esp_idf_nvs_partition_gen','generate',csv_path,bin_path,'0x20000'])
    Path(bin_path).chmod(0o600)

def fresh_hub(port,profile,bundle):
    chip=PROFILES[profile][0];mac=detect(port,profile);require_blank_nvs(port,chip)
    sys.path.insert(0,str(ROOT/'tools'));import provision
    output=ROOT/'secrets'/f'hub-{uuid.uuid4().hex[:12]}'
    provision.generate(argparse.Namespace(hub_mac=provision.mac(mac),node_mac=[],pins=None,country='01',channel=6,wifi_ssid='',wifi_password='',web=True,output=output))
    nvs_generate(output/'hub.csv',output/'hub.nvs.bin',bundle)
    files=load_bundle(bundle,profile)
    if input(f'Первичная установка нового Hub {mac}. Введите INSTALL: ').strip()!='INSTALL': print('Отменено. Плата не изменена.');return
    tool('--chip',chip,'--port',port,'write_flash','0x0',files['bootloader'],'0x8000',files['partition_table'],*initial_ota_args(files),'0x30000',files['app'],'0x9000',output/'hub.nvs.bin')
    print(f'Готово. Подключитесь к SmartPlantOS и откройте https://192.168.4.1/setup\nПароль точки и ключ администратора: {output/"access.json"}')

def update(port,profile,bundle):
    chip=PROFILES[profile][0];detect(port,profile,keep_rom=True);files=load_bundle(bundle,profile)
    manifest=json.loads((Path(bundle)/'manifest.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        directory=Path(directory);path=directory/'partitions.bin';read_region(port,chip,0x8000,0x1000,path,keep_rom=True)
        installed=path.read_bytes();offset,size=parse_partitions(installed)
        if manifest.get('format')==1:
            if installed[:0xc00]!=files['partition_table'].read_bytes()[:0xc00]:raise ValueError('Пакет требует другой разметки')
            if files['app'].stat().st_size>size:raise ValueError('Приложение не помещается в раздел')
            if input('Старое обновление без отката. Введите UPDATE: ').strip()!='UPDATE':print('Отменено.');return
            tool('--chip',chip,'--port',port,'write_flash',hex(offset),files['app']);return
        from ota_layout import selection,selection_data,image_size
        target_size=0x1e0000 if profile=='c3' else 0x200000
        migration=size in (0x300000,0x400000)
        if migration:
            # Preserve the existing factory app as ota_0. Never read/write NVS.
            fallback=directory/'fallback.bin';read_region(port,chip,0x30000,target_size,fallback,keep_rom=True)
            data=fallback.read_bytes();image_size(data)
            if len(data)<128 or data[80:112].split(b'\0',1)[0]!=(b'smart_plant_node' if profile=='c3' else b'smart_plant_hub'):raise ValueError('Установлено неизвестное приложение; автоматическая миграция отменена')
            sequence=1;candidate=0x30000+target_size
        else:
            if installed[:0xc00]!=files['partition_table'].read_bytes()[:0xc00]:raise ValueError('Неизвестная OTA-разметка; запись отменена')
            ota=directory/'old-otadata.bin';read_region(port,chip,0x2a000,8192,ota,keep_rom=True)
            slot,sequence=selection(ota.read_bytes());candidate=0x30000+(1-slot)*target_size
        selection_file=directory/'otadata.bin';selection_file.write_bytes(selection_data(sequence))
        print('USB-миграция в два OTA-слота.' if migration else 'Обновление неактивного OTA-слота.')
        print('NVS, ключи, Wi-Fi, привязки и лимиты сохраняются. Не отключайте USB во время записи загрузчика и таблицы.')
        if input('Введите UPDATE: ').strip()!='UPDATE':print('Отменено. Переподключите USB для запуска прежней прошивки.');return
        tool('--chip',chip,'--port',port,'--after','no_reset','write_flash',hex(candidate),files['app'])
        if migration:
            tool('--chip',chip,'--port',port,'write_flash','0x0',files['bootloader'],'0x8000',files['partition_table'],'0x2a000',selection_file)
        else:
            tool('--chip',chip,'--port',port,'write_flash','0x2a000',selection_file)
    print('Кандидат записан. Подождите 30–90 секунд: новая прошивка подтвердит запуск либо загрузчик вернёт предыдущий слот. Не восстанавливайте старые копии NVS.')

def prepare_node(port,passport_file,bundle):
    passport=json.loads(Path(passport_file).read_text(encoding="utf-8"));profile='c3';chip='esp32c3'
    sys.path.insert(0,str(ROOT/'tools'));import provision
    if set(passport)!= {'format','hub_id','hub_mac','country','channel'} or passport['format']!=1 or not re.fullmatch(r'spo-[0-9a-f]{12}',passport['hub_id']): raise ValueError('Неверный паспорт Hub')
    if not any(bytes.fromhex(passport['hub_id'][4:])): raise ValueError('Пустой ID Hub')
    hubmac=provision.mac(passport['hub_mac'])
    if passport['country'] not in provision.IDF_COUNTRIES or type(passport['channel']) is not int or not 1<=passport['channel']<=11: raise ValueError('Неверные сетевые параметры паспорта')
    mac=detect(port,profile)
    if provision.mac(mac)==hubmac: raise ValueError('MAC C3 совпал с Hub')
    require_blank_nvs(port,chip);files=load_bundle(bundle,profile)
    output=ROOT/'secrets'/f'node-{uuid.uuid4().hex[:12]}'
    output.mkdir(parents=True,mode=0o700,exist_ok=False)
    device=secrets.token_bytes(6);lmk=secrets.token_bytes(16);app=secrets.token_bytes(16)
    voucher={'format':1,'hub_id':passport['hub_id'],'node_id':'spo-'+device.hex(),'node_mac':mac,'lmk':lmk.hex(),'app_key':app.hex()}
    rows=[['key','type','encoding','value'],['provision','namespace','','']]
    fields={'role':('u8',0),'count':('u8',1),'channel':('u8',passport['channel']),'country':('string',passport['country']),'device':('hex2bin',device.hex()),'plant':('hex2bin',uuid.uuid4().hex),'pmk':('hex2bin',secrets.token_hex(16)),'p0mac':('hex2bin',hubmac.hex()),'p0id':('hex2bin',passport['hub_id'][4:]),'p0lmk':('hex2bin',lmk.hex()),'p0app':('hex2bin',app.hex())}
    for key,(encoding,value) in fields.items():rows.append([key,'data',encoding,value])
    for filename,content in [('node-pairing.json',json.dumps(voucher,indent=2))]:
        fd=os.open(output/filename,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'w',encoding='utf-8') as f:f.write(content+'\n')
    fd=os.open(output/'node.csv',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'w',newline='',encoding='utf-8') as f:csv.writer(f).writerows(rows)
    nvs_generate(output/'node.csv',output/'node.nvs.bin',bundle)
    if input(f'Новый C3 {mac}, выходы насоса отключены. Введите INSTALL: ').strip()!='INSTALL':print('Отменено.');return
    tool('--chip',chip,'--port',port,'write_flash','0x0',files['bootloader'],'0x8000',files['partition_table'],*initial_ota_args(files),'0x30000',files['app'],'0x9000',output/'node.nvs.bin')
    print(f'Готово. На странице /setup вашего Hub загрузите {output/"node-pairing.json"}\nЭто приватный файл. GPIO остаются неназначенными.')

def factory_reset(port,profile):
    chip=PROFILES[profile][0];mac=detect(port,profile)
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'partitions.bin';read_region(port,chip,0x8000,0x1000,path)
        data=path.read_bytes();parse_partitions(data)
        found=False
        for offset in range(0,len(data),32):
            row=data[offset:offset+32]
            if len(row)!=32 or struct.unpack_from('<H',row)[0]!=0x50aa:break
            _,kind,subtype,address,size,label,_=struct.unpack('<HBBLL16sL',row)
            if label.rstrip(b'\0')==b'nvs' and kind==1 and subtype==2 and address==0x9000 and size==0x20000:found=True
        if not found:raise ValueError('Не найдена ожидаемая область NVS. Сброс отменён.')
    print('Заводской сброс удалит ключи, привязки, настройки и историю этой платы. Затем нужна новая установка с новыми ключами.')
    if input(f'Для сброса введите RESET {mac}: ').strip()!=f'RESET {mac}':print('Отменено.');return
    after=('--after','no_reset') if profile=='c3' else ()
    tool('--chip',chip,'--port',port,*after,'erase_region','0x9000','0x20000')
    print('NVS очищен. Приложение сохранено. Выполните первую установку; старый NVS не восстанавливайте.')
    if profile=='c3':print('C3 оставлен в загрузчике до новой установки. Не переподключайте USB между сбросом и new-node.')

def setup_ap(port):
    import serial
    connection=serial.Serial(baudrate=115200,timeout=0.2)
    connection.dtr=False;connection.rts=False;connection.port=port
    with connection:
        time.sleep(0.2);connection.write(b'\nsetup-ap\n');deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            line=connection.readline().decode('utf-8',errors='replace').strip()
            if line.startswith('OK setup-ap'):print('Точка SmartPlantOS открыта. Пароль smartplantos. Окно восстановления — 5 минут.');return
            if line.startswith('ERR setup-ap'):raise ValueError(line)
    raise ValueError('Нет ответа. Дождитесь загрузки Hub или обновите прошивку с поддержкой setup-ap.')

def new_node(port,bundle):
    """Install a fresh discoverable C3; firmware creates its own local identity once."""
    profile='c3';chip='esp32c3';detect(port,profile,keep_rom=True);require_blank_nvs(port,chip,keep_rom=True)
    files=load_bundle(bundle,profile)
    if input('Новый C3 без привязки. GPIO отключены. Введите INSTALL: ').strip()!='INSTALL':print('Отменено. Для запуска прежнего приложения переподключите USB.');return
    tool('--chip',chip,'--port',port,'write_flash','0x0',files['bootloader'],'0x8000',files['partition_table'],*initial_ota_args(files),'0x30000',files['app'])
    print('C3 готов. Откройте приложение Hub → Добавить контроллер → Найти рядом. Поиск доступен первые 2 минуты после включения C3.')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',nargs='?',choices=['install','update','prepare-node','ports','factory-reset','setup-ap','new-node'])
    parser.add_argument('--port');parser.add_argument('--profile',choices=PROFILES)
    parser.add_argument('--bundle',type=Path);parser.add_argument('--passport',type=Path)
    args=parser.parse_args()
    if args.action=='ports':
        for p in ports():print(f'{p.device}\t{p.description}')
        return
    if not args.action:
        print('Smart Plant OS\n1. Первая установка Hub\n2. Обновить прошивку с сохранением настроек\n3. Установить новый C3 для автоматического поиска\n4. Заводской сброс платы\n5. Открыть точку доступа Hub по USB')
        args.action={'1':'install','2':'update','3':'new-node','4':'factory-reset','5':'setup-ap'}.get(input('Выберите действие: ').strip())
        if not args.action:raise ValueError('Неверное действие')
    print("USB-проба перезапускает плату. Перед работой отключите питание насоса и других нагрузок.")
    args.port=args.port or choose_port()
    if args.action=='setup-ap':setup_ap(args.port);return
    args.profile='c3' if args.action in {'prepare-node','new-node'} else args.profile or input('Профиль платы (n8r2 / n16r8 / c3): ').strip().lower()
    if args.profile not in PROFILES or (args.action=='install' and args.profile=='c3'):raise ValueError('Выберите n8r2/n16r8 для Hub или prepare-node для нового C3')
    if args.action=='factory-reset':factory_reset(args.port,args.profile);return
    if not args.bundle:
        path=input('Каталог USB-пакета (Enter — собрать из исходников через ESP-IDF): ').strip()
        args.bundle=Path(path) if path else source_build(args.profile)
    load_bundle(args.bundle,args.profile)
    if args.action=='update':update(args.port,args.profile,args.bundle)
    elif args.action=='install':fresh_hub(args.port,args.profile,args.bundle)
    elif args.action=='new-node':new_node(args.port,args.bundle)
    else:prepare_node(args.port,args.passport or input('Путь к hub-passport.json: ').strip(),args.bundle)

if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.CalledProcessError,ImportError) as e:
        print(f'Остановлено: {e}',file=sys.stderr);sys.exit(1)
    except (KeyboardInterrupt,EOFError):
        print('\nПрервано. Если запись уже началась, завершите её повторным USB-обновлением.',file=sys.stderr);sys.exit(130)
