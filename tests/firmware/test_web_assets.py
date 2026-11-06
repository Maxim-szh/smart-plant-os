import importlib.util,re,gzip,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('build_web_assets',ROOT/'tools/build_web_assets.py');assets=importlib.util.module_from_spec(spec);spec.loader.exec_module(assets)
class WebAssets(unittest.TestCase):
 def test_roundtrip_deterministic_and_all_declared_assets(self):
  args=(ROOT/'web',(ROOT/'firmware/components/sp_platform/CMakeLists.txt').read_text(),(ROOT/'firmware/components/sp_platform/sp_web.c').read_text())
  first,summary=assets.build(*args);second,_=assets.build(*args);self.assertEqual(first,second)
  packed={name:bytes(map(int,re.findall(r'\d+',body))) for name,body in re.findall(r'static const uint8_t sp_gz_(\w+)\[\] = \{(.*?)\};',first,re.S)}
  for name,raw_size,encoded_size in summary:
   if encoded_size<raw_size:
    # Each encoded asset must decompress to the exact source, not a minifier rewrite.
    source=(ROOT/'web'/name).read_bytes();self.assertIn(source,[gzip.decompress(b) for b in packed.values()])
  self.assertLess(sum(x[2] for x in summary),sum(x[1] for x in summary)*.6)
  self.assertTrue(any(name=='fonts/onest-cyrillic.woff2' and raw==encoded for name,raw,encoded in summary))
if __name__=='__main__':unittest.main()
