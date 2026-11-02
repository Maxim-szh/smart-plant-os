import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import ota_layout as ota
import flash
import hashlib
import json
import struct
import tempfile
import unittest
from unittest.mock import patch

def table(profile):
    size=0x1e0000 if profile=='c3' else 0x200000
    entries=[(1,2,0x9000,0x20000,b'nvs'),(1,1,0x29000,4096,b'phy_init'),(1,0,0x2a000,8192,b'otadata'),(0,16,0x30000,size,b'ota_0'),(0,17,0x30000+size,size,b'ota_1')]
    if profile!='c3':entries.append((1,0x40,0x430000,0x1e0000,b'update_stage'))
    raw=b''.join(struct.pack('<HBBLL16sL',0x50aa,*e,0) for e in entries)
    return (raw+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(raw).digest()).ljust(0xc00,b'\xff')

def image(profile):
    data=bytearray(256);data[0]=0xe9;data[1]=1;data[3]=0x20 if profile=='c3' else 0x30
    struct.pack_into('<H',data,12,5 if profile=='c3' else 9);struct.pack_into('<II',data,24,0,180)
    struct.pack_into('<I',data,32,0xabcd5432);data[48:61]=b'0.2.0-abi1\0\0\0';name=b'smart_plant_node' if profile=='c3' else b'smart_plant_hub';data[80:80+len(name)]=name
    return bytes(data)

def package(path,profile):
    contents={'app':image(profile),'partition_table':table(profile),'bootloader':b'boot','otadata':b'\xff'*8192}
    m={'format':2,'layout':'ota-v1','data_abi':1,'version':'0.2.0-abi1','profile':profile,'chip':'esp32c3' if profile=='c3' else 'esp32s3','app_offset':0x30000}
    for k,v in contents.items():
        (path/(k+'.bin')).write_bytes(v);m[k]={'file':k+'.bin','size':len(v),'sha256':hashlib.sha256(v).hexdigest()}
    (path/'manifest.json').write_text(json.dumps(m))

class OtaTests(unittest.TestCase):
    def test_crc_matches_official_esp_idf_vector(self):
        # ESP-IDF otatool uses binascii.crc32(pack('<I', seq), 0xffffffff).
        self.assertEqual(ota.entry(1,2).hex(),'01000000'+'ff'*20+'02000000'+'9a984347')
        self.assertEqual(ota.selection(ota.entry(1,2).ljust(4096,b'\xff')+b'\xff'*4096),(0,1))
        for state in (0,1):
            with self.assertRaises(ValueError):ota.selection(ota.entry(2,state).ljust(8192,b'\xff'))
        corrupted=bytearray(ota.entry(1,2).ljust(8192,b'\xff'));corrupted[0]^=1
        with self.assertRaises(ValueError):ota.selection(corrupted)
    def test_new_candidate_and_fallback_map_to_opposite_slots(self):
        for seq in (1,2,7,40):
            values=ota.entries(ota.selection_data(seq));self.assertEqual([(v[0],v[1]) for v in values],[(seq,2),(seq+1,0)])
            self.assertNotEqual((seq-1)%2,seq%2)
    def test_layouts_and_image_bounds(self):
        for profile in ('n8r2','c3'):
            self.assertEqual(flash.parse_partitions(table(profile))[0],0x30000);self.assertLess(ota.image_size(image(profile)),256)
        data=bytearray(image('c3'));struct.pack_into('<I',data,28,0x400000)
        with self.assertRaises(ValueError):ota.image_size(data)
    def test_usb_migration_preserves_nvs_and_fallback_and_orders_writes(self):
        for profile in ('n8r2','c3'):
            with self.subTest(profile=profile),tempfile.TemporaryDirectory() as tmp:
                path=Path(tmp);package(path,profile)
                rows=b''.join(struct.pack('<HBBLL16sL',0x50aa,*e,0) for e in [(1,2,0x9000,0x20000,b'nvs'),(1,1,0x29000,4096,b'phy_init'),(0,0,0x30000,0x300000 if profile=='c3' else 0x400000,b'factory')])
                legacy=(rows+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(rows).digest()).ljust(4096,b'\xff')
                memory=bytearray(b'\xff'*(8*1024*1024));memory[0x8000:0x9000]=legacy;memory[0x9000:0x29000]=b'\x71'*0x20000;memory[0x30000:0x30100]=image(profile)
                nvs=bytes(memory[0x9000:0x29000]);fallback=bytes(memory[0x30000:0x30100]);writes=[]
                def tool(*args,**kwargs):
                    if 'read_flash' in args:
                        i=args.index('read_flash');a,n=int(args[i+1],16),int(args[i+2],16);Path(args[i+3]).write_bytes(memory[a:a+n])
                    elif 'write_flash' in args:
                        i=args.index('write_flash');pairs=args[i+1:];addresses=[]
                        for k in range(0,len(pairs),2):
                            a=int(pairs[k],16);v=Path(pairs[k+1]).read_bytes();memory[a:a+len(v)]=v;addresses.append(a)
                        writes.append(addresses)
                    else:self.fail(str(args))
                with patch.object(flash,'detect'),patch.object(flash,'tool',side_effect=tool),patch('builtins.input',return_value='UPDATE'):flash.update('test',profile,path)
                self.assertEqual(writes,[[0x210000 if profile=='c3' else 0x230000],[0,0x8000,0x2a000]])
                self.assertEqual(memory[0x9000:0x29000],nvs);self.assertEqual(memory[0x30000:0x30100],fallback)
if __name__=='__main__':unittest.main()
