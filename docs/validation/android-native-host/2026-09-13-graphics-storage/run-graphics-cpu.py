import hashlib,json,pathlib,re,subprocess,time
root=pathlib.Path.cwd(); out=root/'build/wp2-review/graphics-cpu-final';out.mkdir(exist_ok=False)
adb=['/Users/bytedance/Library/Android/sdk/platform-tools/adb','-s','9c2841a4'];remote='/data/local/tmp/shadps4-wp2-cpu-'+str(time.time_ns())
def run(args,timeout=90):
    r=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
    if r.returncode: raise RuntimeError(str(args)+r.stdout+r.stderr)
    return r.stdout
manifest={'source':run(['git','rev-parse','HEAD']).strip(),'remote':remote,'scope':'auxiliary CLI regression','runs':[]}
try:
    manifest['device']={p:run(adb+['shell','getprop',p]).strip() for p in ('ro.product.model','ro.build.version.sdk')}
    manifest['pagesize']=run(adb+['shell','getconf','PAGESIZE']).strip()
    run(adb+['shell','mkdir',remote])
    stl=root/'build/android-host-api33/native/libshadps4_host.so'
    files=[root/'build/wp1-review/device'/n for n in ('guest_execution_tests','guest_cpu_contract_tests','guest_cpu_hle_abi_tests','hle_registration_tests','veneer_allocator_tests','guest_services_tests','session_lifecycle_tests')]
    files.append(pathlib.Path('/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'))
    expected=['244 check(s), ALL PASS (0 failures)','46/46 passed','14/14 passed','13 checks, 0 failures','19 check(s), ALL PASS (0 failures)','GUEST_SERVICES checks=70 failures=0','840 checks, 0 failures']
    for p in files:
        sha=hashlib.sha256(p.read_bytes()).hexdigest(); run(adb+['push',str(p),remote+'/'+p.name])
        assert run(adb+['shell','sha256sum',remote+'/'+p.name]).split()[0]==sha
    for p,marker in zip(files,expected):
        run(adb+['shell','chmod','700',remote+'/'+p.name])
        cmd=adb+['shell',f'LD_LIBRARY_PATH={remote} timeout -s KILL 75 {remote}/{p.name}']
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=85)
        text=result.stdout+result.stderr;(out/(p.name+'.log')).write_text(text)
        ok=result.returncode==0 and text.count(marker)==1
        manifest['runs'].append({'suite':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'exit':result.returncode,'marker':marker,'pass':ok})
        assert ok,(p.name,text[-1000:])
    manifest['status']='PASS'
except Exception as e:
    manifest['status']='FAIL';manifest['error']=str(e);raise
finally:
    c=subprocess.run(adb+['shell','rm','-rf',remote],capture_output=True,text=True)
    manifest['cleanup_exit']=c.returncode
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
