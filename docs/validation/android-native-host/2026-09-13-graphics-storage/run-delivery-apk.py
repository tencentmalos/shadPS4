import hashlib,json,pathlib,re,subprocess,time,zipfile,tempfile,shlex
root=pathlib.Path.cwd();out=root/'build/wp2-review/graphics-storage-delivery';out.mkdir(exist_ok=False)
adb=['/Users/bytedance/Library/Android/sdk/platform-tools/adb','-s','9c2841a4']
def run(args,timeout=180):
 r=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
 if r.returncode:raise RuntimeError(str(args)+r.stdout+r.stderr)
 return r.stdout
sha=lambda p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
m={'source':run(['git','rev-parse','HEAD']).strip(),'source_status':run(['git','status','--short']),'scope':'ordinary APK graphics + storage synthetic acceptance; real TMNT boundary observation only','runs':[]}
try:
 apk=root/'android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk'
 test=root/'android/shadps4-app/app/build/outputs/apk/androidTest/playstore/debug/app-playstore-debug-androidTest.apk'
 m['artifacts']={str(p.relative_to(root)):sha(p) for p in (apk,test,root/'build/android-host-api33/native/libshadps4_host.so')}
 m['changed_sources']={p:sha(root/p) for p in run(['git','ls-files','--modified','--others','--exclude-standard','src','tests','android','scripts','CMakeLists.txt']).splitlines() if (root/p).is_file()}
 llvm=pathlib.Path('/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin')
 def bid(path):return re.search(r'Build ID: (\w+)',run([str(llvm/'llvm-readelf'),'-n',str(path)])).group(1)
 with zipfile.ZipFile(apk) as z,tempfile.TemporaryDirectory() as td:
  for name in ('libshadps4_host.so','libshadps4_fex_session.so'):
   b=z.read('lib/arm64-v8a/'+name);p=pathlib.Path(td)/name;p.write_bytes(b)
   m[name]={'build_id':bid(p),'sha256':hashlib.sha256(b).hexdigest()}
   if name=='libshadps4_host.so':assert bid(p)==bid(root/'build/android-host-api33/native/libshadps4_host.so')
 run(adb+['install','-r',str(apk)]);run(adb+['install','-r',str(test)])
 rel=json.loads((root/'build/wp2-review/tmnt-graphics-first/selected-content.json').read_text())['relative']
 suites=[('graphics','RenderedRuntimeInstrumentedTest#syntheticVideoOutValidatesStackArgumentsFaultsAndRestarts',[]),('storage-write','StorageRuntimeInstrumentedTest',['-e','storageStage','write']),('storage-read','StorageRuntimeInstrumentedTest',['-e','storageStage','read']),('tmnt-boundary','RenderedRuntimeInstrumentedTest#realContentUsesSessionRendererAcrossThreeRestarts',['-e','contentRelativePath',rel])]
 ids={}
 for name,selector,args in suites:
  run(adb+['shell','am','force-stop','com.shadps4.android'])
  start=run(adb+['shell',"date '+%m-%d %H:%M:%S.000'"]).strip()
  output=run(adb+['shell','am','instrument','-w','-r','-e','class','com.shadps4.android.'+selector]+args+['com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner'])
  (out/(name+'-junit.log')).write_text(output)
  logs=run(adb+['logcat','-d','-v','threadtime','-T',start,'-s','StorageRuntimeAcceptance:I','RenderedRuntimeAcceptance:I','ProductionRuntime:I','AndroidRuntime:E','DEBUG:I','*:S'])
  (out/(name+'-logcat.txt')).write_text(logs)
  assert 'OK (1 test)' in output and 'FAILURES!!!' not in output,output[-2500:]
  if name=='graphics':
   lines=[l for l in logs.splitlines() if 'RenderedRuntimeAcceptance: synthetic round=' in l]
   assert len(lines)==6,lines
   assert sum('case=gpu-flip ' in l and 'outcome=0 ' in l for l in lines)==3
   assert sum(int(n)>=4 for n in re.findall(r'guest_presents=(\d+)',logs))==3
  elif name.startswith('storage'):
   lines=[l for l in logs.splitlines() if 'StorageRuntimeAcceptance: stage=' in l]
   assert len(lines)==(3 if name=='storage-write' else 2),lines
   assert all('outcome=0 ' in l and 'guest return=51966' in l for l in lines),lines
   ids[name]=set(re.findall(r'pid=(\d+)', '\n'.join(lines)))
   assert len(ids[name])==1
   if name=='storage-read':assert ids[name]!=ids['storage-write']
  else:
   lines=[l for l in logs.splitlines() if 'RenderedRuntimeAcceptance: round=' in l]
   assert len(lines)==3 and all('import=wtkt-teR1so#libScePosix#' in l and 'outcome=2 ' in l for l in lines),lines
  m['runs'].append({'suite':name,'result':'BOUNDARY_OBSERVED_NOT_GAME_PASS' if name=='tmnt-boundary' else 'PASS','lines':lines})
  print(name,m['runs'][-1]['result'],flush=True)
 m['status']='SYNTHETIC_PASS_TMNT_BLOCKED'
except Exception as e:m['status']='FAIL';m['error']=str(e);raise
finally:(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
