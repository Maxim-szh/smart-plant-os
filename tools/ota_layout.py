"""Smart Plant OS ota-v1 format. ESP-IDF otadata CRC covers sequence only."""
import binascii
import struct

def entry(sequence,state):
    if not 1<=sequence<0xfffffffe or state not in (0,1,2,3,4):raise ValueError('Invalid OTA selection')
    crc=binascii.crc32(struct.pack('<I',sequence),0xffffffff)&0xffffffff
    return struct.pack('<I20sII',sequence,b'\xff'*20,state,crc)

def entries(raw):
    if len(raw)!=8192:raise ValueError('Truncated otadata')
    result=[]
    for sector in (0,4096):
        sequence,_,state,crc=struct.unpack('<I20sII',raw[sector:sector+32])
        if sequence in (0,0xffffffff) or crc!=(binascii.crc32(struct.pack('<I',sequence),0xffffffff)&0xffffffff):continue
        result.append((sequence,state,sector))
    return result

def selection(raw):
    values=entries(raw)
    if any(s in (0,1) for _,s,_ in values):raise ValueError('Прошивка ещё не подтвердила запуск. Подождите 30 секунд и повторите.')
    valid=[e for e in values if e[1]==2]
    if not valid:raise ValueError('Нет подтверждённого OTA-слота. Используйте USB-восстановление; не стирайте NVS.')
    current=max(valid);return (current[0]-1)%2,current[0]

def selection_data(previous_sequence):
    """Keep a valid fallback plus NEW candidate, each in its own flash sector."""
    if previous_sequence>=0xfffffffc:raise ValueError('OTA sequence exhausted')
    return entry(previous_sequence,2).ljust(4096,b'\xff')+entry(previous_sequence+1,0).ljust(4096,b'\xff')

def image_size(data):
    if len(data)<24 or data[0]!=0xe9 or not 1<=data[1]<=16:raise ValueError('Неверный установленный ESP-образ')
    end=24
    for _ in range(data[1]):
        if end+8>len(data):raise ValueError('Прежний образ не помещается в новый слот')
        length=struct.unpack_from('<I',data,end+4)[0];end+=8+length
        if end>len(data):raise ValueError('Прежний образ не помещается в новый слот')
    end=(end+16)&~15
    if data[23]==1:end+=32
    elif data[23]!=0:raise ValueError('Unsupported image hash flag')
    if end>len(data):raise ValueError('Прежний образ не помещается в новый слот')
    return end
