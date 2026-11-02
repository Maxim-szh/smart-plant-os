import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sp_flash',ROOT/'tools/flash.py')
flash = importlib.util.module_from_spec(spec);spec.loader.exec_module(flash)

def table(address=0x30000):
    rows = b''.join(struct.pack('<HBBLL16sL',*entry) for entry in [
        (0x50aa,1,2,0x9000,0x20000,b'nvs',0),
        (0x50aa,1,1,0x29000,0x1000,b'phy_init',0),
        (0x50aa,0,0,address,0x400000,b'factory',0)])
    return (rows+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(rows).digest()).ljust(0xc00,b'\xff')

def bundle(directory):
    app = bytearray(100);app[0]=0xe9;app[3]=0x30;struct.pack_into('<H',app,12,9)
    contents={'app':bytes(app),'bootloader':b'bootloader','partition_table':table()}
    manifest={'format':1,'profile':'n8r2','chip':'esp32s3','app_offset':0x30000}
    for key,data in contents.items():
        filename=key+'.bin';(directory/filename).write_bytes(data)
        manifest[key]={'file':filename,'sha256':hashlib.sha256(data).hexdigest()}
    (directory/'manifest.json').write_text(json.dumps(manifest))
    return manifest

class FlashTests(unittest.TestCase):
    def test_partition_md5_and_factory_address(self):
        self.assertEqual(flash.parse_partitions(table()),(0x30000,0x400000))
        corrupted=bytearray(table());corrupted[10]^=1
        with self.assertRaises(ValueError):flash.parse_partitions(corrupted)
        with self.assertRaises(ValueError):flash.parse_partitions(table(0x40000))
        with self.assertRaises(ValueError):flash.parse_partitions(b'\xff'*4096)

    def test_profile_hash_chip_and_path_checks(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp);manifest=bundle(path)
            self.assertTrue(flash.load_bundle(path,'n8r2')['app'].is_file())
            with self.assertRaises(ValueError):flash.load_bundle(path,'c3')
            (path/'app.bin').write_bytes(b'bad')
            with self.assertRaises(ValueError):flash.load_bundle(path,'n8r2')
            bundle(path);manifest['app']['file']='../app.bin'
            (path/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):flash.load_bundle(path,'n8r2')

    def test_update_only_app_and_preserves_private_regions(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp);bundle(path)
            memory=bytearray(b'\xff'*(8*1024*1024));memory[0x8000:0x8c00]=table()
            memory[0x9000:0x29000]=b'\x31'*0x20000
            before=bytes(memory[:0x30000]);writes=[]
            def tool(*args,**kwargs):
                if 'read_flash' in args:
                    i=args.index('read_flash');offset=int(args[i+1],16);size=int(args[i+2],16)
                    Path(args[i+3]).write_bytes(memory[offset:offset+size])
                elif 'write_flash' in args:
                    i=args.index('write_flash');offset=int(args[i+1],16);data=Path(args[i+2]).read_bytes()
                    writes.append(offset);memory[offset:offset+len(data)]=data
                else:self.fail(f'Unexpected operation {args}')
            with patch.object(flash,'detect',return_value='02:00:00:00:00:01'),patch.object(flash,'tool',side_effect=tool),patch('builtins.input',return_value='UPDATE'):
                flash.update('test','n8r2',path)
            self.assertEqual(writes,[0x30000]);self.assertEqual(bytes(memory[:0x30000]),before)

    def test_explicit_factory_reset_erases_only_nvs(self):
        memory=bytearray(b'\x42'*0x440000);memory[0x8000:0x8c00]=table()
        before=bytes(memory);commands=[]
        def tool(*args,**kwargs):
            if 'read_flash' in args:
                i=args.index('read_flash');offset=int(args[i+1],16);size=int(args[i+2],16)
                Path(args[i+3]).write_bytes(memory[offset:offset+size])
            elif 'erase_region' in args:
                i=args.index('erase_region');address=int(args[i+1],16);size=int(args[i+2],16)
                commands.append((address,size));memory[address:address+size]=b'\xff'*size
            else:self.fail(str(args))
        with patch.object(flash,'detect',return_value='02:00:00:00:00:01'),patch.object(flash,'tool',side_effect=tool):
            with patch('builtins.input',return_value='cancel'):flash.factory_reset('test','n8r2')
            self.assertEqual(commands,[])
            with patch('builtins.input',return_value='RESET 02:00:00:00:00:01'):flash.factory_reset('test','n8r2')
        self.assertEqual(commands,[(0x9000,0x20000)])
        self.assertEqual(memory[:0x9000],before[:0x9000]);self.assertEqual(memory[0x29000:],before[0x29000:])
        self.assertEqual(memory[0x9000:0x29000],b'\xff'*0x20000)

    def test_discoverable_node_installs_no_private_partition(self):
        files={key:Path(key+'.bin') for key in ['app','bootloader','partition_table']}
        with patch.object(flash,'detect'),patch.object(flash,'require_blank_nvs') as blank,patch.object(flash,'load_bundle',return_value=files),patch.object(flash,'tool') as tool:
            with patch('builtins.input',return_value='cancel'):flash.new_node('test',Path('bundle'))
            tool.assert_not_called()
            with patch('builtins.input',return_value='INSTALL'):flash.new_node('test',Path('bundle'))
            blank.assert_called_with('test','esp32c3',keep_rom=True)
            args=tool.call_args.args
            self.assertIn('0x30000',args);self.assertNotIn('0x9000',args);self.assertNotIn('erase_region',args)

    def test_fresh_node_probes_never_boot_autoprovisioning_app(self):
        from types import SimpleNamespace
        memory=bytearray(b'\xff'*0x20000);writes=[];reads=[]
        files={key:Path(key+'.bin') for key in ['app','bootloader','partition_table']}
        def tool(*args,**kwargs):
            if 'read_flash' in args:
                i=args.index('read_flash');self.assertEqual(args[i+1:i+3],('0x9000','0x20000'))
                Path(args[i+3]).write_bytes(memory);reads.append('nvs')
                output=''
            elif 'flash_id' in args:reads.append('flash');output='Detected flash size: 4MB'
            elif 'read_mac' in args:reads.append('mac');output='MAC: 02:00:00:00:00:01'
            elif 'write_flash' in args:writes.append(args);output=''
            else:self.fail(str(args))
            # Firmware creates identity on every first boot of a blank partition.
            if '--after' not in args or args[args.index('--after')+1]!='no_reset':memory[0]=0
            return SimpleNamespace(stdout=output)
        with patch.object(flash,'load_bundle',return_value=files),patch.object(flash,'tool',side_effect=tool),patch('builtins.input',return_value='INSTALL'):
            flash.new_node('test',Path('bundle'))
        self.assertEqual(reads,['flash','mac','nvs']);self.assertEqual(len(writes),1)
        self.assertEqual(memory[0],0)  # Only the final successful write boots the app.

    def test_fresh_install_rejects_nonempty_nvs(self):
        def read(port,chip,address,size,path):Path(path).write_bytes(b'\xff'*5+b'\x00')
        with patch.object(flash,'read_region',side_effect=read):
            with self.assertRaises(ValueError):flash.require_blank_nvs('test','esp32c3')

if __name__=='__main__':unittest.main()
